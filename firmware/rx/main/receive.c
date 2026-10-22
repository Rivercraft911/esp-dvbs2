// Continuous receive: core 1 demodulates every unit into a frame ring; core 0 checks or
// decodes each frame and streams one binary record per frame over USB.
#include <stdio.h>
#include <string.h>
#include "receive.h"

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "capture.h"
#include "dvbs2rx.h"

#define SLOTS 6U
#define STOP 0xffU

typedef struct {
    rx_demod_t *demod;
    rx_fec_t *fec;
    rx_frame_t slot[SLOTS], *current;
    QueueHandle_t full, empty;
    SemaphoreHandle_t done;
    receive_stats_t *stats;
} receiver_t;

/* Core 1, inside the capture loop: bounded work, no blocking. */
static esp_err_t IRAM_ATTR consume(const uint32_t *words, size_t count, uint64_t first, void *ctx)
{
    receiver_t *r = ctx;
    receive_stats_t *s = r->stats;
    uint32_t start = esp_cpu_get_cycle_count();
    if (rx_demod_push(r->demod, words, first, count, r->current, 1)) {
        uint8_t next, done = r->current - r->slot;
        /* Without a free slot the finished frame is dropped and its buffers reused. */
        if (xQueueReceive(r->empty, &next, 0) == pdTRUE) {
            xQueueSend(r->full, &done, 0);
            r->current = &r->slot[next];
        } else {
            ++s->dropped;
        }
    }
    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    s->push_cycles += cycles;
    if (cycles > s->max_push_cycles) s->max_push_cycles = cycles;
    return ESP_OK;
}

static void decoder(void *arg)
{
    receiver_t *r = arg;
    receive_stats_t *s = r->stats;
    uint8_t i;
    while (xQueueReceive(r->full, &i, portMAX_DELAY) == pdTRUE && i != STOP) {
        rx_frame_t *f = &r->slot[i];
        const uint8_t *payload = NULL;
        rx_fec_stats_t fec = {0};
        uint32_t start = esp_cpu_get_cycle_count();
        int length = rx_fec_frame(r->fec, f, &payload, &fec);
        uint32_t cycles = esp_cpu_get_cycle_count() - start;
        s->fec_cycles += cycles;
        if (cycles > s->max_fec_cycles) s->max_fec_cycles = cycles;
        if (fec.iterations > s->max_iterations) s->max_iterations = fec.iterations;
        ++s->frames;
        length >= 0 ? ++s->good : ++s->failed;
        receive_record_t rec = {RECEIVE_MAGIC, f->sequence, f->cfo_hz, f->mer_cdb,
                                (int16_t)length, fec.iterations, fec.failed_checks};
        usb_serial_jtag_write_bytes(&rec, sizeof rec, portMAX_DELAY);
        if (length > 0) usb_serial_jtag_write_bytes(payload, length, portMAX_DELAY);
        xQueueSend(r->empty, &i, portMAX_DELAY);
    }
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

esp_err_t receive_run(const capture_config_t *capture, unsigned modcod, receive_stats_t *s,
                      capture_stats_t *cs)
{
    memset(s, 0, sizeof *s);
    receiver_t r = {.stats = s};
    rx_config_t config = {.modcod = modcod, .cfo_hint_hz = -10000, .cfo_span_hz = 30000};
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    esp_err_t error = ESP_ERR_NO_MEM;
    r.demod = heap_caps_aligned_alloc(16, rx_demod_size(), internal);
    r.fec = heap_caps_aligned_alloc(16, rx_fec_size(modcod), internal);
    r.full = xQueueCreate(SLOTS + 1, 1);
    r.empty = xQueueCreate(SLOTS, 1);
    r.done = xSemaphoreCreateBinary();
    bool ready = r.demod && r.fec && r.full && r.empty && r.done;
    for (unsigned i = 0; ready && i < SLOTS; ++i) {
        r.slot[i].hard = heap_caps_aligned_alloc(16, RX_HARD_BYTES, internal);
        r.slot[i].llr = heap_caps_aligned_alloc(16, RX_SHORT_BITS, MALLOC_CAP_SPIRAM);
        ready = r.slot[i].hard && r.slot[i].llr;
        uint8_t n = i;
        if (ready && i) xQueueSend(r.empty, &n, 0);
    }
    r.current = &r.slot[0];
    if (!ready) goto done;
    error = ESP_ERR_INVALID_ARG;
    if (rx_demod_init(r.demod, &config) || rx_fec_init(r.fec, modcod)) goto done;
    error = ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(decoder, "decoder", 6144, &r, 10, NULL, 0) != pdPASS) goto done;
    printf("@{\"cmd\":\"rx\",\"stream\":\"binary\",\"record_bytes\":%u}\n",
           (unsigned)sizeof(receive_record_t));
    fflush(stdout);
    error = capture_run(capture, consume, &r, cs);
    uint8_t stop = STOP;
    xQueueSend(r.full, &stop, portMAX_DELAY);
    xSemaphoreTake(r.done, portMAX_DELAY);
    receive_record_t end = {RECEIVE_MAGIC, 0, 0, 0, RECEIVE_END, 0, 0};
    usb_serial_jtag_write_bytes(&end, sizeof end, portMAX_DELAY);
    rx_demod_stats(r.demod, &s->demod);

done:
    for (unsigned i = 0; i < SLOTS; ++i) {
        heap_caps_free(r.slot[i].hard);
        heap_caps_free(r.slot[i].llr);
    }
    if (r.done) vSemaphoreDelete(r.done);
    if (r.empty) vQueueDelete(r.empty);
    if (r.full) vQueueDelete(r.full);
    heap_caps_free(r.fec);
    heap_caps_free(r.demod);
    return error;
}
