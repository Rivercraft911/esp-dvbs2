// The encoder consumes USB RX during a binary session; IDF retains console TX.
// Re-mask RX and drain the driver queue before EVERY FIFO read. A driver's
// interrupt-enable read/modify/write can restore RX after our initial mask.
#include "usb_input.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_select.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_heap_caps.h"
#include "run_memory.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static ingress_ring ring;
static uint8_t *buffers;
static uint32_t *tables;
static size_t partial;
static bool ended, active, saved_rx_enabled;
static uint64_t wire_bytes;
static uint32_t fifo_bytes, driver_bytes, fifo_reads, waits, timer_errors;
static uint32_t rx_reenabled, driver_queue_max;
static int64_t first, last;
// First 64 seconds, relative to ready. No logging on either timed core. These
// distinguish a full output ring during CW from a reader starved after CW.
#define TRACE_SECONDS 64
static struct { uint32_t reads, payload_bytes; } trace[TRACE_SECONDS];
static int64_t trace_start;
static esp_timer_handle_t timer;
static TaskHandle_t waiter;
static portMUX_TYPE rx_mux=portMUX_INITIALIZER_UNLOCKED;

static unsigned trace_second(void)
{
    return (esp_timer_get_time()-trace_start)/1000000;
}

// Driver ISR and encoder are both on core 0. Protect the shared enable register
// against the TX ISR's read/modify/write, without disabling console TX.
static void rx_interrupt(bool enable)
{
    portENTER_CRITICAL(&rx_mux);
    if (enable) usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    else usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    portEXIT_CRITICAL(&rx_mux);
}

static void wake(void *arg) { (void)arg; xTaskNotifyGive(waiter); }
static void ready(void *ctx, int level) { (void)ctx; (void)level; }
static void rearm(void *ctx, ingress_desc *slot, bool restart)
{
    (void)ctx; (void)slot; (void)restart;
}
static const ingress_hw hw = {.ready=ready, .rearm=rearm};

static void free_buffers(void)
{
    if (timer) (void)esp_timer_stop_blocking(timer,portMAX_DELAY); // kept for the next run
    pv_crc32_tables(NULL);
    tables=NULL; buffers=NULL; active=false; // run memory, returned with the run
}

static bool (*control_handler)(unsigned);
void usb_input_control(bool (*control)(unsigned)) { control_handler=control; }

// At boot: the wake timer is the input's only heap allocation, made once and kept.
esp_err_t usb_input_init(void)
{
    const esp_timer_create_args_t cfg={.callback=wake,.name="usb_rx"};
    return timer ? ESP_OK : esp_timer_create(&cfg,&timer);
}

esp_err_t usb_input_start(unsigned type)
{
    buffers=run_take(INGRESS_SLOTS*INGRESS_SLOT_BYTES, 16, MALLOC_CAP_SPIRAM);
    tables=run_take(4096, 16, MALLOC_CAP_INTERNAL);
    if (!buffers || !tables || !timer) {
        free_buffers(); return timer ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&rx_mux);
    saved_rx_enabled=(usb_serial_jtag_ll_get_intr_ena_status() & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT)!=0;
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    portEXIT_CRITICAL(&rx_mux);
    pv_crc32_tables(tables);
    ingress_init(&ring,buffers,&hw);
    ring.control=control_handler; ring.type=type;
    partial=0; ended=false; active=true;
    wire_bytes=0; fifo_bytes=driver_bytes=fifo_reads=waits=timer_errors=0; first=last=0;
    rx_reenabled=driver_queue_max=0;
    memset(trace,0,sizeof trace); trace_start=esp_timer_get_time();
    printf("@{\"cmd\":\"usbinput\",\"ready\":true,\"message_bytes\":1332,\"rx\":\"driver+fifo\"}\n");
    return ESP_OK;
}

static size_t read_bytes(uint8_t *dst, size_t n)
{
    const unsigned second=trace_second();
    if (second<TRACE_SECONDS) trace[second].reads++;
    // Console TX uses the driver's unlocked interrupt-enable RMW. A task
    // preempted before our initial RX mask can later store its old RX bit.
    // Exclude the core-0 USB ISR throughout this ordered, nonblocking handoff:
    // queued bytes are older than the hardware FIFO and must be consumed first.
    portENTER_CRITICAL(&rx_mux);
    if (usb_serial_jtag_ll_get_intr_ena_status() & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) rx_reenabled++;
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    const int from_driver=usb_serial_jtag_read_bytes(dst,n,0);
    size_t got;
    if (from_driver>0) {
        got=from_driver; driver_bytes+=got;
        const size_t queued=got+usb_serial_jtag_get_read_bytes_available();
        if (queued>driver_queue_max) driver_queue_max=queued;
    } else {
        // Match IDF's endpoint service order: acknowledge, then drain at most
        // one 64-byte packet. Never clear an event AFTER draining its FIFO.
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        fifo_reads++;
        got=usb_serial_jtag_ll_read_rxfifo(dst,n<64?n:64);
        fifo_bytes+=got;
    }
    portEXIT_CRITICAL(&rx_mux);
    return got;
}

static void wait_packet(void)
{
    // A full USB packet is only 64 bytes. A one-tick poll would cap intake near
    // 0.5 Mb/s. A short timer sleep wakes the encoder without occupying core 1
    // or spinning beside the PSRAM encoder. RX runs INLINE in the encoder on
    // core 0, not in a lower-priority reader task. This notification blocks the
    // task; output-slot waits also block. The timed core-1 fill never waits on
    // USB: it uses dummy frames if the output queue is empty.
    waiter=xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE,0);
    if (esp_timer_start_once(timer,50)!=ESP_OK) { timer_errors++; vTaskDelay(1); return; }
    ulTaskNotifyTake(pdTRUE,1);
    esp_timer_stop_blocking(timer,portMAX_DELAY); // also handles unrelated slot/stop notifications
    waits++;
}

// Assemble a fixed PV message across FIFO packets. Bound each call even if a
// source trickles forever, so the encoder's stop flag is revisited promptly.
static bool receive(void)
{
    ingress_desc *d=&ring.desc[ring.head%INGRESS_SLOTS];
    const int64_t deadline=esp_timer_get_time()+5000;
    do {
        const size_t n=read_bytes(d->buf+partial,PV_MSG_BYTES-partial);
        wire_bytes+=n; partial+=n;
        if (partial==PV_MSG_BYTES) break;
        if (!n) wait_packet();
    } while (esp_timer_get_time()<deadline);
    if (partial!=PV_MSG_BYTES) return false;
    partial=0;
    const uint16_t seq=d->buf[4] | d->buf[5]<<8;
    const bool forward=!ring.seq_known || (uint16_t)(seq-ring.seq_next)<0x8000;
    if (forward && d->buf[3]==PV_NOP && pv_check(d->buf,PV_MSG_BYTES)==PV_OK) ended=true;
    const int64_t now=esp_timer_get_time();
    if (!first) first=now;
    last=now;
    d->dw0=INGRESS_SLOT_BYTES | (PV_MSG_BYTES<<12) | INGRESS_EOF;
    return true;
}

size_t usb_input_read(uint8_t *dst, size_t n)
{
    size_t got=0;
    while (got<n) {
        const size_t copied=ingress_read(&ring,dst+got,n-got);
        const unsigned second=trace_second();
        if (second<TRACE_SECONDS) trace[second].payload_bytes+=copied;
        got+=copied;
        if (got==n || ended || !receive()) break;
    }
    return got;
}

void usb_input_stop(void) { /* RX stays masked through final counters and drain. */ }

void usb_input_print(void)
{
    const ingress_stats *s=&ring.st;
    printf(",\"controls\":%"PRIu32",\"control_rejected\":%"PRIu32, s->controls,s->control_rejected);
    const int64_t us=last>first ? last-first : 0;
    const uint32_t rx_mask=usb_serial_jtag_ll_get_intr_ena_status();
    const size_t pending=usb_serial_jtag_get_read_bytes_available();
    printf(",\"input_wire\":\"usb\",\"input_bytes\":%"PRIu32
           ",\"usb\":{\"messages\":%"PRIu32",\"payload_bytes\":%"PRIu32
           ",\"nops\":%"PRIu32",\"short\":%"PRIu32",\"header\":%"PRIu32
           ",\"crc\":%"PRIu32",\"sync\":%"PRIu32",\"lost\":%"PRIu32
           ",\"back\":%"PRIu32",\"wrong_type\":%"PRIu32
           ",\"next_seq\":%u,\"ts_packets\":%"PRIu32",\"crc_chain\":%"PRIu32
           ",\"wire_bytes\":%"PRIu64",\"partial_bytes\":%u,\"ended\":%s,\"us\":%lld,\"mbps\":%.3f"
           ",\"fifo_bytes\":%"PRIu32",\"driver_bytes\":%"PRIu32",\"fifo_reads\":%"PRIu32",\"waits\":%"PRIu32",\"timer_errors\":%"PRIu32
           ",\"rx_reenabled\":%"PRIu32",\"driver_queue_max\":%"PRIu32",\"driver_pending\":%u,\"intr_ena\":%"PRIu32,
           s->bytes,s->msgs,s->bytes,s->nops,s->err_short,s->err_header,s->err_crc,s->err_sync,
           s->lost,s->seq_back,ring.wrong_type,ring.seq_next,s->ts_packets,s->crc_chain,
           wire_bytes,(unsigned)partial,ended?"true":"false",(long long)us,
           us ? s->bytes*8.0/us : 0.0,fifo_bytes,driver_bytes,fifo_reads,waits,timer_errors,rx_reenabled,driver_queue_max,(unsigned)pending,rx_mask);
    const int64_t elapsed=esp_timer_get_time()-trace_start;
    unsigned count=elapsed/1000000+1;
    if (count>TRACE_SECONDS) count=TRACE_SECONDS;
    printf(",\"last_message_us\":%lld,\"trace_elapsed_us\":%lld,\"trace\":[",
           (long long)(last ? last-trace_start : 0),(long long)elapsed);
    for (unsigned i=0;i<count;++i) {
        printf("%s{\"reads\":%"PRIu32",\"payload_bytes\":%"PRIu32"}",
               i ? "," : "",trace[i].reads,trace[i].payload_bytes);
    }
    printf("]}");
}

void usb_input_finish(void)
{
    if (!active) return;
    printf("@{\"cmd\":\"usbinput\",\"stop\":true}\n");
    const uint64_t before=wire_bytes;
    const int64_t limit=esp_timer_get_time()+3000000;
    while (!ended && esp_timer_get_time()<limit) {
        if (!receive()) vTaskDelay(1);
    }
    printf("@{\"cmd\":\"usbinput\",\"ended\":%s,\"drained_bytes\":%"PRIu64",\"partial_bytes\":%u%s}\n",
           ended?"true":"false",wire_bytes-before,(unsigned)partial,
           ended?"":",\"error\":\"missing terminal PV NOP\",\"recovery\":\"restart\"");
    const bool restart=!ended;
    free_buffers();
    if (restart) {
        // RF and the encoder have already stopped. Never pass a partial binary
        // session to the line parser. Flush the diagnostic record, then reboot
        // into a clean console; the stored callsign remains in NVS.
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }
    rx_interrupt(saved_rx_enabled);
}
