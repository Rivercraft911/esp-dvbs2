// One reader: the encoder consumes USB directly, so its queue provides USB
// backpressure without a producer task or a second payload copy.
#include "usb_input.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_heap_caps.h"
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
static bool ended, active;
static uint64_t wire_bytes;
static int64_t first, last;
static portMUX_TYPE rx_mux=portMUX_INITIALIZER_UNLOCKED;

// IDF's ISR drains the hardware FIFO even when xRingbufferSendFromISR fails.
// Keep RX masked between complete messages/encoder frames so a full encoder
// queue makes the USB endpoint NAK. TX interrupts and the console driver stay
// active. Both the driver ISR (installed by app_main) and encoder run on core 0;
// exclude that ISR while changing its shared interrupt-enable register.
static void rx_interrupt(bool enable)
{
    portENTER_CRITICAL(&rx_mux);
    if (enable) usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    else usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    portEXIT_CRITICAL(&rx_mux);
}

static void ready(void *ctx, int level) { (void)ctx; (void)level; }
static void rearm(void *ctx, ingress_desc *slot, bool restart)
{
    (void)ctx; (void)slot; (void)restart;
}
static const ingress_hw hw = {.ready=ready, .rearm=rearm};

esp_err_t usb_input_start(unsigned type)
{
    buffers=heap_caps_malloc(INGRESS_SLOTS*INGRESS_SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tables=heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffers || !tables) {
        heap_caps_free(buffers); heap_caps_free(tables);
        buffers=NULL; tables=NULL;
        return ESP_ERR_NO_MEM;
    }
    rx_interrupt(false);
    pv_crc32_tables(tables);
    ingress_init(&ring,buffers,&hw); ring.type=type;
    partial=0; ended=false; active=true; wire_bytes=0; first=last=0;
    printf("@{\"cmd\":\"usbinput\",\"ready\":true,\"message_bytes\":1332}\n");
    return ESP_OK;
}

// A USB stream has no CS edges: assemble exactly 1332 bytes across arbitrary
// driver reads. Invalid messages are discarded at this fixed boundary by ingress.
static bool receive(void)
{
    ingress_desc *d=&ring.desc[ring.head%INGRESS_SLOTS];
    while (partial<PV_MSG_BYTES) {
        int n=usb_serial_jtag_read_bytes(d->buf+partial,PV_MSG_BYTES-partial,0);
        if (n<=0) {
            // Only unmask an empty software queue. Block on that queue for at
            // most one tick: a USB packet wakes us immediately, without a
            // millisecond polling gap for each 64-byte endpoint packet.
            rx_interrupt(true);
            n=usb_serial_jtag_read_bytes(d->buf+partial,PV_MSG_BYTES-partial,1);
            if (n<=0) return false;
        }
        wire_bytes+=n; partial+=n;
    }
    rx_interrupt(false);
    partial=0;
    const uint16_t seq=d->buf[4] | d->buf[5]<<8;
    const bool forward=!ring.seq_known || (uint16_t)(seq-ring.seq_next)<0x8000;
    const bool nop=forward && d->buf[3]==PV_NOP && pv_check(d->buf,PV_MSG_BYTES)==PV_OK;
    if (nop) ended=true; // USB-only end of stream; its wire format is an ordinary PV NOP.
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

void usb_input_stop(void) { if (active) rx_interrupt(false); /* Storage lives through command-task drain. */ }

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
           ",\"wire_bytes\":%"PRIu64",\"partial_bytes\":%u,\"ended\":%s,\"us\":%lld,\"mbps\":%.3f}",
           s->bytes,s->msgs,s->bytes,s->nops,s->err_short,s->err_header,s->err_crc,s->err_sync,
           s->lost,s->seq_back,ring.wrong_type,ring.seq_next,s->ts_packets,s->crc_chain,
           wire_bytes,(unsigned)partial,ended?"true":"false",(long long)us,
           us ? s->bytes*8.0/us : 0.0);
}

void usb_input_finish(void)
{
    if (!active) return;
    // No encoder or raw IQ writer is alive here. This record tells a timed
    // sender to finish its current message, send NOP, and stop writing.
    printf("@{\"cmd\":\"usbinput\",\"stop\":true}\n");
    const int64_t limit=esp_timer_get_time()+3000000;
    while (!ended && esp_timer_get_time()<limit) {
        // Discard queued payload after the run, preserving any partial message.
        if (!receive()) vTaskDelay(1);
    }
    printf("@{\"cmd\":\"usbinput\",\"ended\":%s%s}\n",ended?"true":"false",
           ended?"":",\"error\":\"missing terminal PV NOP; reset before a new session\"");
    // On a failed handoff, quarantine input until a valid NOP arrives. A late
    // writer (or a reconnect) can never turn arbitrary payload into RF commands.
    while (!ended) { if (!receive()) vTaskDelay(1); }
    pv_crc32_tables(NULL);
    heap_caps_free(tables); heap_caps_free(buffers);
    tables=NULL; buffers=NULL; active=false;
    rx_interrupt(true); // return sole RX ownership to the VFS console
}
