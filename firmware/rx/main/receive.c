// Continuous receive. Core 1 pushes every capture unit into the demodulator, which queues data
// runs. On core 0, three tasks: the release turns queued runs into frames of hard and weak
// bits (highest priority: it frees the ring core 1 writes); the checker runs each frame's
// re-encode check and streams one binary record per frame over USB; the decoder runs the LDPC
// decoder on frames that fail the check, one at a time, in PSRAM. A replay drives the same
// receiver from a recorded capture instead of the radio.
#include <stdio.h>
#include <string.h>
#include "receive.h"

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "capture.h"
#include "dvbs2rx.h"

#define SLOTS 3U
#define PIECE 3072U             /* words per push: the release starts within ~1,000 symbols */
#define STACK 4096U
#define STOP 0xffU
#define UNIT_CYCLES (CAPTURE_UNIT * 15U)   /* 240 MHz / 16 MS/s */
#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef struct {
    rx_demod_t *demod;
    void *ring, *work;
    rx_fec_t *check, *decode;   /* the clean path (internal); the decoder (PSRAM) */
    rx_config_t config;
    bool crc;
    rx_frame_t slot[SLOTS];
    rx_frame_t job;             /* the decoder's frame, copied to PSRAM */
    volatile bool busy, stop;
    QueueHandle_t full, empty;
    SemaphoreHandle_t done, usb;
    TaskHandle_t release, checker, decoder;
    receive_stats_t *stats;
    const int8_t *sc8;          /* replay */
    size_t pairs;
    unsigned skip;
} receiver_t;

/* The driver's transmit ring holds 8 KB: larger blocks go in pieces. */
void receive_write(const void *data, size_t count)
{
    for (size_t sent = 0; sent < count;) {
        int n = usb_serial_jtag_write_bytes((const uint8_t *)data + sent,
                                            count - sent > 4096 ? 4096 : count - sent, portMAX_DELAY);
        if (n > 0) sent += n;
    }
}

/* Core 1: bounded work, no blocking. Pieces of PIECE words, waking the release after any that
 * queued a run, so it never trails a whole capture unit. */
static esp_err_t IRAM_ATTR consume(const uint32_t *words, size_t count, uint64_t first, void *ctx)
{
    receiver_t *r = ctx;
    receive_stats_t *s = r->stats;
    uint32_t start = esp_cpu_get_cycle_count();
    for (size_t k = 0; k < count; k += PIECE) {
        size_t n = count - k < PIECE ? count - k : PIECE;
        if (rx_demod_push(r->demod, words + k, first + k, n)) xTaskNotifyGive(r->release);
    }
    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    s->push_cycles += cycles;
    if (cycles > s->max_push_cycles) {
        s->max_push_cycles = cycles;
        s->max_push_at = first;
    }
    if (s->pushes < RECEIVE_LOG) s->log[s->pushes] = cycles;
    ++s->pushes;
    return ESP_OK;
}

static void quit(receiver_t *r, uint32_t *stack_free)
{
    *stack_free = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(r->done);
    vTaskSuspend(NULL);         /* finish() deletes it, so its static buffers are free again */
}

/* Highest priority on core 0: every queued run, into the current slot; a whole frame goes to
 * the checker. Without a free slot the frame is dropped and its buffers reused. */
static void releaser(void *arg)
{
    receiver_t *r = arg;
    receive_stats_t *s = r->stats;
    uint8_t cur = 0, next;
    while (!r->stop) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t start = esp_cpu_get_cycle_count();
            int done = rx_demod_release(r->demod, &r->slot[cur]);
            uint32_t cycles = esp_cpu_get_cycle_count() - start;
            s->release_cycles += cycles;
            if (cycles > s->max_release_cycles) s->max_release_cycles = cycles;
            if (!done) break;
            if (xQueueReceive(r->empty, &next, 0) == pdTRUE) {
                xQueueSend(r->full, &cur, 0);
                cur = next;
            } else {
                ++s->dropped;
            }
        }
    }
    quit(r, &s->release_stack_free);
}

static void record(receiver_t *r, const rx_frame_t *f, int length, const uint8_t *payload,
                   const rx_fec_stats_t *fec)
{
    receive_stats_t *s = r->stats;
    receive_record_t rec = {RECEIVE_MAGIC, f->sequence, f->cfo_hz, f->mer_cdb, (int16_t)length,
                            fec->iterations, fec->failed_checks};
    xSemaphoreTake(r->usb, portMAX_DELAY);
    ++s->frames;
    length >= 0 ? ++s->good : ++s->failed;
    if (fec->iterations > s->max_iterations) s->max_iterations = fec->iterations;
    receive_write(&rec, sizeof rec);
    if (length > 0 && r->crc) {
        uint32_t crc = esp_rom_crc32_le(0, payload, length);
        receive_write(&crc, 4);
    } else if (length > 0) {
        receive_write(payload, length);
    } else if (length < 0) {
        /* A failed frame carries its first sample and bits for host diagnosis. */
        receive_write(&f->header_sample, 8);
        receive_write(f->hard, RECEIVE_BITS);
        receive_write(f->weak, RECEIVE_BITS);
    }
    xSemaphoreGive(r->usb);
}

/* The clean path for every frame; a frame that fails its parity checks goes to the decoder
 * if it is free, or out as failed. */
static void checker(void *arg)
{
    receiver_t *r = arg;
    receive_stats_t *s = r->stats;
    uint8_t i;
    while (xQueueReceive(r->full, &i, portMAX_DELAY) == pdTRUE && i != STOP) {
        rx_frame_t *f = &r->slot[i];
        const uint8_t *payload = NULL;
        rx_fec_stats_t fec = {0};
        uint32_t start = esp_cpu_get_cycle_count();
        int length = rx_fec_frame(r->check, f, &payload, &fec);
        uint32_t cycles = esp_cpu_get_cycle_count() - start;
        s->fec_cycles += cycles;
        if (cycles > s->max_fec_cycles) s->max_fec_cycles = cycles;
        if (length < 0 && fec.failed_checks && !r->busy) {
            uint8_t *hard = r->job.hard, *weak = r->job.weak;
            r->job = *f;
            r->job.hard = hard;
            r->job.weak = weak;
            memcpy(hard, f->hard, RX_HARD_BYTES);
            memcpy(weak, f->weak, RX_HARD_BYTES);
            r->busy = true;
            xTaskNotifyGive(r->decoder);
        } else {
            record(r, f, length, payload, &fec);
        }
        xQueueSend(r->empty, &i, portMAX_DELAY);
    }
    quit(r, &s->check_stack_free);
}

static void decoder(void *arg)
{
    receiver_t *r = arg;
    receive_stats_t *s = r->stats;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!r->busy) break;                    /* the stop */
        const uint8_t *payload = NULL;
        rx_fec_stats_t fec = {0};
        int length = rx_fec_frame(r->decode, &r->job, &payload, &fec);
        s->decoded += length >= 0;
        record(r, &r->job, length, payload, &fec);
        r->busy = false;
    }
    quit(r, &s->decode_stack_free);
}

/* Every run allocation (after Wi-Fi has gone, internal RAM for what core 1 and the release
 * touch), the core 0 tasks and the stream header. */
static esp_err_t start(void *ctx)
{
    receiver_t *r = ctx;
    const unsigned modcod = r->config.modcod;
    r->demod = heap_caps_aligned_alloc(16, rx_demod_size(), INTERNAL);
    r->ring = heap_caps_aligned_alloc(16, RX_DEMOD_RING_BYTES, INTERNAL);
    r->check = heap_caps_aligned_alloc(16, rx_fec_check_size(modcod), INTERNAL);
    r->decode = heap_caps_aligned_alloc(16, rx_fec_size(modcod), MALLOC_CAP_SPIRAM);
    r->work = heap_caps_aligned_alloc(16, rx_fec_work_size(modcod), MALLOC_CAP_SPIRAM);
    r->job.hard = heap_caps_aligned_alloc(16, RX_HARD_BYTES, MALLOC_CAP_SPIRAM);
    r->job.weak = heap_caps_aligned_alloc(16, RX_HARD_BYTES, MALLOC_CAP_SPIRAM);
    r->full = xQueueCreate(SLOTS + 1, 1);
    r->empty = xQueueCreate(SLOTS, 1);
    r->done = xSemaphoreCreateCounting(3, 0);
    r->usb = xSemaphoreCreateMutex();
    bool ok = r->demod && r->ring && r->check && r->decode && r->work && r->job.hard &&
              r->job.weak && r->full && r->empty && r->done && r->usb;
    for (unsigned i = 0; ok && i < SLOTS; ++i) {
        r->slot[i].hard = heap_caps_aligned_alloc(16, RX_HARD_BYTES, INTERNAL);
        r->slot[i].weak = heap_caps_aligned_alloc(16, RX_HARD_BYTES, INTERNAL);
        ok = r->slot[i].hard && r->slot[i].weak;
        uint8_t n = i;
        if (ok && i) xQueueSend(r->empty, &n, 0);   /* the release starts on slot 0 */
    }
    if (!ok) return ESP_ERR_NO_MEM;
    if (rx_demod_init(r->demod, &r->config, r->ring) || rx_fec_init(r->check, modcod, NULL) ||
        rx_fec_init(r->decode, modcod, r->work))
        return ESP_ERR_INVALID_ARG;
    /* Static stacks: never in slow RTC FAST memory. */
    static StackType_t stacks[3][STACK / sizeof(StackType_t)];
    static StaticTask_t tasks[3];
    r->release = xTaskCreateStaticPinnedToCore(releaser, "release", STACK, r, 12, stacks[0], &tasks[0], 0);
    r->checker = xTaskCreateStaticPinnedToCore(checker, "check", STACK, r, 11, stacks[1], &tasks[1], 0);
    r->decoder = xTaskCreateStaticPinnedToCore(decoder, "decode", STACK, r, 5, stacks[2], &tasks[2], 0);
    if (!r->release || !r->checker || !r->decoder) return ESP_ERR_NO_MEM;
    r->stats->free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    r->stats->largest_internal = heap_caps_get_largest_free_block(INTERNAL);
    printf("@{\"cmd\":\"rx\",\"stream\":\"binary\",\"record_bytes\":%u,\"payload\":\"%s\","
           "\"failed\":\"hard+weak\"}\n", (unsigned)sizeof(receive_record_t), r->crc ? "crc32" : "bytes");
    fflush(stdout);
    return ESP_OK;
}

/* A task that has finished (suspended itself), or one that never ran past its first wait. */
static void reap(TaskHandle_t task, bool finished)
{
    if (!task) return;
    while (finished && eTaskGetState(task) != eSuspended) vTaskDelay(1);
    vTaskDelete(task);
}

/* Stop the core 0 tasks after the last frames, close the stream and free everything. */
static void finish(receiver_t *r)
{
    receive_stats_t *s = r->stats;
    const bool running = r->release && r->checker && r->decoder;
    if (running) {
        r->stop = true;
        xTaskNotifyGive(r->release);
        xSemaphoreTake(r->done, portMAX_DELAY);
        uint8_t stop = STOP;
        xQueueSend(r->full, &stop, portMAX_DELAY);
        xSemaphoreTake(r->done, portMAX_DELAY);
        while (r->busy) vTaskDelay(1);
        xTaskNotifyGive(r->decoder);
        xSemaphoreTake(r->done, portMAX_DELAY);
        receive_record_t end = {RECEIVE_MAGIC, 0, 0, 0, RECEIVE_END, 0, 0};
        receive_write(&end, sizeof end);
        rx_demod_stats(r->demod, &s->demod);
    }
    reap(r->release, running);
    reap(r->checker, running);
    reap(r->decoder, running);
    for (unsigned i = 0; i < SLOTS; ++i) {
        heap_caps_free(r->slot[i].hard);
        heap_caps_free(r->slot[i].weak);
    }
    if (r->usb) vSemaphoreDelete(r->usb);
    if (r->done) vSemaphoreDelete(r->done);
    if (r->empty) vQueueDelete(r->empty);
    if (r->full) vQueueDelete(r->full);
    heap_caps_free(r->job.hard);
    heap_caps_free(r->job.weak);
    heap_caps_free(r->work);
    heap_caps_free(r->decode);
    heap_caps_free(r->check);
    heap_caps_free(r->ring);
    heap_caps_free(r->demod);
}

static void setup(receiver_t *r, unsigned modcod, unsigned rate, unsigned mhz, receive_stats_t *s)
{
    memset(s, 0, sizeof *s);
    *r = (receiver_t){.stats = s, .crc = rate == 5000000,
                      .config = {.modcod = modcod, .symbol_rate = rate, .cfo_hint_hz = -10000,
                                 .cfo_span_hz = 30000, .carrier_khz = mhz * 1000U}};
}

esp_err_t receive_run(const capture_config_t *capture, unsigned modcod, unsigned rate,
                      receive_stats_t *s, capture_stats_t *cs)
{
    receiver_t r;
    setup(&r, modcod, rate, capture->mhz, s);
    capture_config_t c = *capture;
    c.ready = start;
    esp_err_t error = capture_run(&c, consume, &r, cs);
    finish(&r);
    return error;
}

/* Core 1: the capture as dump words in capture bank 0, a capture unit at a time, each pushed
 * when its samples would have arrived. */
static void pusher(void *arg)
{
    receiver_t *r = arg;
    uint32_t *words = CAPTURE_BANK(0), t0 = esp_cpu_get_cycle_count();
    for (size_t first = 0, unit = 1; first < r->pairs; first += CAPTURE_UNIT, ++unit) {
        size_t n = r->pairs - first < CAPTURE_UNIT ? r->pairs - first : CAPTURE_UNIT;
        if (r->skip && unit % r->skip == 0) continue;
        /* The dump word's I bits 19:12 and Q bits 9:2 are exactly what was recorded. */
        for (size_t k = 0; k < n; ++k)
            words[k] = (uint32_t)(uint8_t)r->sc8[2 * (first + k)] << 12 |
                       (uint32_t)(uint8_t)r->sc8[2 * (first + k) + 1] << 2;
        while ((int32_t)(esp_cpu_get_cycle_count() - t0 - (uint32_t)(unit * UNIT_CYCLES)) < 0) {
        }
        consume(words, n, first, r);
    }
    xSemaphoreGive(r->done);
    vTaskSuspend(NULL);
}

esp_err_t receive_replay(const int8_t *sc8, size_t pairs, unsigned modcod, unsigned rate,
                         unsigned skip, receive_stats_t *s)
{
    receiver_t r;
    setup(&r, modcod, rate, 2420, s);
    r.sc8 = sc8;
    r.pairs = pairs;
    r.skip = skip;
    esp_err_t error = start(&r);
    if (error == ESP_OK) {
        static StackType_t stack[STACK / sizeof(StackType_t)];
        static StaticTask_t task;
        TaskHandle_t push = xTaskCreateStaticPinnedToCore(pusher, "replay", STACK, &r,
                                                          configMAX_PRIORITIES - 1, stack, &task, 1);
        if (push) {
            xSemaphoreTake(r.done, portMAX_DELAY);
            reap(push, true);
        } else {
            error = ESP_ERR_NO_MEM;
        }
    }
    finish(&r);
    return error;
}
