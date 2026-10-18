// The encoder owns USB RX during a binary session. IDF keeps console TX, but
// its RX ISR is masked because it drops bytes when its software queue fills.
// Resuming an already nonempty FIFO never depends on a new interrupt edge.
#include "usb_input.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_heap_caps.h"
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
static bool ended, active, driver_empty;
static uint64_t wire_bytes;
static uint32_t fifo_bytes, driver_bytes, fifo_reads, waits, timer_errors;
static int64_t first, last;
static esp_timer_handle_t timer;
static TaskHandle_t waiter;
static portMUX_TYPE rx_mux=portMUX_INITIALIZER_UNLOCKED;

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
    if (timer) {
        esp_timer_stop_blocking(timer,portMAX_DELAY);
        esp_timer_delete(timer); timer=NULL;
    }
    pv_crc32_tables(NULL);
    heap_caps_free(tables); heap_caps_free(buffers);
    tables=NULL; buffers=NULL; active=false;
}

esp_err_t usb_input_start(unsigned type)
{
    buffers=heap_caps_malloc(INGRESS_SLOTS*INGRESS_SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tables=heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const esp_timer_create_args_t cfg={.callback=wake,.name="usb_rx"};
    esp_err_t err=ESP_ERR_NO_MEM;
    if (!buffers || !tables || (err=esp_timer_create(&cfg,&timer))!=ESP_OK) {
        free_buffers(); return err;
    }
    rx_interrupt(false);
    pv_crc32_tables(tables);
    ingress_init(&ring,buffers,&hw); ring.type=type;
    partial=0; ended=false; active=true; driver_empty=false;
    wire_bytes=0; fifo_bytes=driver_bytes=fifo_reads=waits=timer_errors=0; first=last=0;
    printf("@{\"cmd\":\"usbinput\",\"ready\":true,\"message_bytes\":1332,\"rx\":\"fifo\"}\n");
    return ESP_OK;
}

static size_t read_bytes(uint8_t *dst, size_t n)
{
    // Preserve any bytes queued before the console-to-binary handoff. Once
    // drained, the driver never reads RX again until this session is finished.
    if (!driver_empty) {
        const int got=usb_serial_jtag_read_bytes(dst,n,0);
        if (got>0) { driver_bytes+=got; return got; }
        driver_empty=true;
    }
    fifo_reads++;
    const size_t got=usb_serial_jtag_ll_read_rxfifo(dst,n);
    fifo_bytes+=got;
    // Poll data_avail itself, never the packet interrupt edge. Clear stale RX
    // status while masked; future console packets can raise a fresh event.
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    return got;
}

static void wait_packet(void)
{
    // A full USB packet is only 64 bytes. A one-tick poll would cap intake near
    // 0.5 Mb/s. A short timer sleep wakes the encoder without occupying core 1
    // or spinning beside the PSRAM encoder. No timer runs during encoding.
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
        got+=ingress_read(&ring,dst+got,n-got);
        if (got==n || ended || !receive()) break;
    }
    return got;
}

void usb_input_stop(void) { /* RX stays masked through final counters and drain. */ }

void usb_input_print(void)
{
    const ingress_stats *s=&ring.st;
    const int64_t us=last>first ? last-first : 0;
    printf(",\"input_wire\":\"usb\",\"input_bytes\":%"PRIu32
           ",\"usb\":{\"messages\":%"PRIu32",\"payload_bytes\":%"PRIu32
           ",\"nops\":%"PRIu32",\"short\":%"PRIu32",\"header\":%"PRIu32
           ",\"crc\":%"PRIu32",\"sync\":%"PRIu32",\"lost\":%"PRIu32
           ",\"back\":%"PRIu32",\"wrong_type\":%"PRIu32
           ",\"next_seq\":%u,\"ts_packets\":%"PRIu32",\"crc_chain\":%"PRIu32
           ",\"wire_bytes\":%"PRIu64",\"partial_bytes\":%u,\"ended\":%s,\"us\":%lld,\"mbps\":%.3f"
           ",\"fifo_bytes\":%"PRIu32",\"driver_bytes\":%"PRIu32",\"fifo_reads\":%"PRIu32",\"waits\":%"PRIu32",\"timer_errors\":%"PRIu32"}",
           s->bytes,s->msgs,s->bytes,s->nops,s->err_short,s->err_header,s->err_crc,s->err_sync,
           s->lost,s->seq_back,ring.wrong_type,ring.seq_next,s->ts_packets,s->crc_chain,
           wire_bytes,(unsigned)partial,ended?"true":"false",(long long)us,
           us ? s->bytes*8.0/us : 0.0,fifo_bytes,driver_bytes,fifo_reads,waits,timer_errors);
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
    rx_interrupt(true);
}
