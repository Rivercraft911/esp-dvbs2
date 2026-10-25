// Continuous receive. Core 1 pushes every capture unit into the demodulator, which queues data
// runs. On core 0, three tasks: the release turns queued runs into frames of hard and weak
// bits (highest priority: it frees the ring core 1 writes); the checker runs each frame's
// re-encode check and streams one binary record per frame over USB; the decoder runs the LDPC
// decoder on frames that fail the check, one at a time, in PSRAM. A replay drives the same
// receiver from a recorded capture instead of the radio (capture_replay). In TS mode the checker
// then deframes good and repaired frames in sequence order for the egress (ts_egress.h), holding
// later frames behind decodes until the ordering buffer fills. USB records may be out of order.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "receive.h"

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "arena.h"
#include "capture.h"
#include "dvbs2rx.h"
#include "ingress_pattern.h"
#include "receive_buffers.h"

#define SLOTS 3U
#define PIECE 3072U             /* words per push: the release starts within ~1,000 symbols */
#define STACK 2560U             /* the deepest task used about 1.2 KB */
#define STOP 0xffU
#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef struct {
    uint8_t index;
    int length;
    const uint8_t *payload;    /* within the job, which the checker frees after taking the result */
} decode_result_t;

typedef struct {
    rx_demod_t *demod;
    void *ring, *work;
    rx_fec_t *check, *decode;   /* the clean path (internal); the decoder (PSRAM, but for scratch) */
    void *scratch;              /* the decoder's per-layer scratch (internal) */
    rx_config_t config;
    bool crc;
    rx_frame_t slot[SLOTS];
    decode_job_t *job;          /* JOBS frames and their bit planes, in PSRAM */
    rx_order_t *order;          /* TS frames behind outstanding decodes, also in PSRAM */
    unsigned free_jobs, pending; /* owned by the checker, including results not taken yet */
    volatile bool stop;
    QueueHandle_t full, empty, jobs, results;
    SemaphoreHandle_t done, usb;
    TaskHandle_t release, checker, decoder;
    uint32_t *crc_table;        /* 5 Msym/s: the record's CRC-32 by bytes (PSRAM) */
    receive_stats_t *stats;
    receive_options_t options;
    rx_ts_t *ts;
    int32_t last;               /* pattern: the previous packet's seq << 3 | j; -1 for none */
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
 * the checker. Without a free slot it waits for the checker to free one, at least 1 ms (two ticks):
 * the frames after a lock come in a burst while the first check runs cold (300k to 370k cycles).
 * Meanwhile the demodulator's ring fills (0.82 ms of symbols at 5 Msym/s); if it overflows, the
 * next frame is lost instead of this one. Then the frame is dropped and its buffers reused. */
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
            if (xQueueReceive(r->empty, &next, 2) == pdTRUE) {
                xQueueSend(r->full, &cur, 0);
                cur = next;
            } else {
                ++s->dropped;
            }
        }
    }
    quit(r, &s->release_stack_free);
}

/* zlib's CRC-32 (reflected 0xedb88320, as esp_rom_crc32_le(0, ...)) a byte at a time from a table:
 * the ROM's takes 30k to 90k cycles for an 8PSK 8/9 frame, a third of the frame period. */
static uint32_t crc32(const uint32_t *table, const uint8_t *p, size_t n)
{
    uint32_t c = ~0U;
    while (n--) c = table[(c ^ *p++) & 255] ^ c >> 8;
    return ~c;
}

static void record(receiver_t *r, const rx_frame_t *f, int length, const uint8_t *payload,
                   const rx_fec_stats_t *fec)
{
    receive_stats_t *s = r->stats;
    receive_record_t rec = {RECEIVE_MAGIC, f->sequence, f->cfo_hz, f->mer_cdb, (int16_t)length,
                            fec->iterations, fec->failed_checks};
    rx_monitor_t *m = r->options.mon;
    if (m) {
        m->mer_cdb = f->mer_cdb;
        m->cfo_hz = f->cfo_hz;
        m->clock_ppb = f->clock_ppb;
        m->frames++;
    }
    const uint32_t crc = length > 0 && r->crc ? crc32(r->crc_table, payload, length) : 0;
    xSemaphoreTake(r->usb, portMAX_DELAY);
    ++s->frames;
    length >= 0 ? ++s->good : ++s->failed;
    if (fec->iterations > s->max_iterations) s->max_iterations = fec->iterations;
    const int64_t start = esp_timer_get_time();
    receive_write(&rec, sizeof rec);
    if (length > 0 && r->crc) {
        receive_write(&crc, 4);
    } else if (length > 0) {
        receive_write(payload, length);
    } else if (length < 0) {
        /* A failed frame carries its first sample and bits for host diagnosis. */
        receive_write(&f->header_sample, 8);
        receive_write(f->hard, RECEIVE_BITS);
        receive_write(f->weak, RECEIVE_BITS);
    }
    const uint32_t us = (uint32_t)(esp_timer_get_time() - start);
    if (us > s->max_write_us) {
        s->max_write_us = us;
        s->max_write_sequence = f->sequence;
    }
    s->slow_writes += us >= 1000;
    xSemaphoreGive(r->usb);
}

/* A packet of the transmitter's test input: bytes 4..6 name its message and place (ingress_pattern).
 * The next packet follows it in the message, or opens the next message; `ts replay` loops back
 * from message 7 to 0, a live sender's count wraps at 65,536. */
static void expect(receiver_t *r, const uint8_t *p)
{
    receive_pattern_t *s = &r->stats->pattern;
    const uint32_t seq = p[4] | (uint32_t)p[5] << 8, j = p[6];
    ++s->checked;
    if (r->last >= 0) {
        const uint32_t ls = (uint32_t)r->last >> 3, lj = r->last & 7;
        if (lj == 6 && ls == 7 && !seq && !j) ++s->wraps;
        else if (lj < 6 ? seq != ls || j != lj + 1 : seq != ((ls + 1) & 0xffff) || j) ++s->jumps;
    }
    r->last = (int32_t)(seq << 3 | (j & 7));
    s->bad += ingress_pattern_ts_differs(p, (uint16_t)seq, j) != 0;
}

/* Each packet the deframer hands on: the pattern check if asked, then the egress. Only the
 * checker task deframes, as ts_egress.h promises the egress. */
static void emit(void *ctx, const uint8_t *packet)
{
    receiver_t *r = ctx;
    assert(xTaskGetCurrentTaskHandle() == r->checker);
    if (r->options.pattern) {
        uint32_t start = esp_cpu_get_cycle_count();
        expect(r, packet);
        r->stats->pattern.cycles += esp_cpu_get_cycle_count() - start;
    }
    const ts_egress_t *e = r->options.egress;
    if (e) e->packet(e->ctx, packet);
}

static void deframe(void *ctx, uint32_t sequence, uint8_t *bbframe, int length)
{
    receiver_t *r = ctx;
    assert(xTaskGetCurrentTaskHandle() == r->checker);
    /* A gap has no bytes. The next good frame's sequence makes rx_ts_frame resync, exactly
     * as on the direct path; rx_ts_flush handles a gap at the end of the run. */
    if (!bbframe) return;
    receive_stats_t *s = r->stats;
    uint32_t start = esp_cpu_get_cycle_count();
    rx_ts_frame(r->ts, bbframe, (unsigned)length, sequence);
    const ts_egress_t *e = r->options.egress;
    if (e && e->flush) e->flush(e->ctx);
    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    s->ts_cycles += cycles;
    if (cycles > s->max_ts_cycles) s->max_ts_cycles = cycles;
}

static void completed(receiver_t *r, const decode_result_t *result)
{
    if (r->order) {
        const rx_frame_t *f = &r->job[result->index].frame;
        rx_order_result(r->order, f->sequence,
                        result->length >= 0 ? result->payload - 10 : NULL, result->length);
    }
    r->free_jobs |= 1U << result->index;
    --r->pending;
    if (r->order) rx_order_drain(r->order);
}

/* The clean path for every frame; failed parity checks go to the FIFO decoder while a job
 * is free. Only this task takes decode results and hands TS frames to the deframer. */
static void checker(void *arg)
{
    receiver_t *r = arg;
    receive_stats_t *s = r->stats;
    uint8_t i;
    for (;;) {
        decode_result_t result;
        while (xQueueReceive(r->results, &result, 0) == pdTRUE) completed(r, &result);
        /* One tick lets an idle receiver service results too, without starving the decoder. */
        if (xQueueReceive(r->full, &i, 1) != pdTRUE) continue;
        if (i == STOP) break;
        rx_frame_t *f = &r->slot[i];
        const uint8_t *payload = NULL;
        rx_fec_stats_t fec = {0};
        uint32_t start = esp_cpu_get_cycle_count();
        int length = rx_fec_frame(r->check, f, &payload, &fec);
        uint32_t cycles = esp_cpu_get_cycle_count() - start;
        s->fec_cycles += cycles;
        if (cycles > s->max_fec_cycles) s->max_fec_cycles = cycles;
        if (length < 0 && fec.failed_checks && r->free_jobs) {
            uint8_t j = 0;
            while (!(r->free_jobs & (1U << j))) ++j;
            decode_job_t *job = &r->job[j];
            job->frame = *f;
#define COPY(name) job->frame.name = f->name ? job->name : NULL; \
                   if (f->name) memcpy(job->name, f->name, RX_HARD_BYTES);
            JOB_PLANES(COPY)
#undef COPY
            r->free_jobs &= ~(1U << j);
            ++r->pending;
            if (r->order) rx_order_wait(r->order, f->sequence);
            BaseType_t sent = xQueueSend(r->jobs, &j, 0);
            assert(sent == pdTRUE);
        } else {
            record(r, f, length, payload, &fec);
            if (r->order) rx_order_frame(r->order, f->sequence,
                                        length >= 0 ? f->hard : NULL, length);
        }
        xQueueSend(r->empty, &i, portMAX_DELAY);
    }
    /* The checker must remain alive until every result is taken and deframed. */
    while (r->pending) {
        decode_result_t result;
        xQueueReceive(r->results, &result, portMAX_DELAY);
        completed(r, &result);
    }
    if (r->order) {
        rx_order_drain(r->order);
        rx_order_stats(r->order, &s->order);
    }
    if (r->ts) {                /* the end: a packet waiting for its CRC-8 goes on unchecked */
        rx_ts_flush(r->ts);
        const ts_egress_t *e = r->options.egress;
        if (e && e->flush) e->flush(e->ctx);
    }
    quit(r, &s->check_stack_free);
}

static void decoder(void *arg)
{
    receiver_t *r = arg;
    receive_stats_t *s = r->stats;
    uint8_t i;
    while (xQueueReceive(r->jobs, &i, portMAX_DELAY) == pdTRUE && i != STOP) {
        rx_frame_t *f = &r->job[i].frame;
        const uint8_t *payload = NULL;
        rx_fec_stats_t fec = {0};
        int length = rx_fec_frame(r->decode, f, &payload, &fec);
        s->decoded += length >= 0;
        record(r, f, length, payload, &fec);
        decode_result_t result = {.index = i, .length = length, .payload = payload};
        xQueueSend(r->results, &result, portMAX_DELAY);
    }
    quit(r, &s->decode_stack_free);
}

/* The queues and semaphores, made once at boot from static storage (receive_init). */
static QueueHandle_t full_q, empty_q, jobs_q, results_q;
static SemaphoreHandle_t done_sem, usb_sem;

void receive_init(void)
{
    static StaticQueue_t full_s, empty_s, jobs_s, results_s;
    static StaticSemaphore_t done_s, usb_s;
    static uint8_t full_items[SLOTS + 1], empty_items[SLOTS];
    static uint8_t jobs_items[JOBS + 1], results_items[JOBS * sizeof(decode_result_t)];
    full_q = xQueueCreateStatic(SLOTS + 1, 1, full_items, &full_s);
    empty_q = xQueueCreateStatic(SLOTS, 1, empty_items, &empty_s);
    jobs_q = xQueueCreateStatic(JOBS + 1, 1, jobs_items, &jobs_s);
    results_q = xQueueCreateStatic(JOBS, sizeof(decode_result_t), results_items, &results_s);
    done_sem = xSemaphoreCreateCountingStatic(3, 0, &done_s);
    usb_sem = xSemaphoreCreateMutexStatic(&usb_s);
}

/* Every run's memory, from the arenas (internal RAM for what core 1 and the release touch, PSRAM
 * for the decoder, its jobs and the ordering buffer, but the decoder's per-layer scratch), the core
 * 0 tasks and the stream header. */
static esp_err_t start(void *ctx)
{
    receiver_t *r = ctx;
    const unsigned modcod = r->config.modcod;
    const bool four = r->config.symbol_rate != 5000000;    /* 0 or 1,000,000: 1 Msym/s */
    arena_reset();
    r->demod = arena_take(&arena_low, rx_demod_size());
    r->check = arena_take(&arena_low, rx_fec_check_size(modcod));
    if (r->options.ts) r->ts = arena_take(&arena_low, rx_ts_size());
    r->ring = arena_take(&arena_high, RX_DEMOD_RING_BYTES);
    r->decode = arena_take(&arena_psram, rx_fec_size(modcod));
    r->work = arena_take(&arena_psram, rx_fec_work_size(modcod));
    r->scratch = arena_take(&arena_fast, rx_fec_scratch_size(modcod));
    r->job = arena_take(&arena_psram, JOBS * sizeof *r->job);
    if (r->ts) r->order = arena_take(&arena_psram, rx_order_size(ORDER_FRAMES));
    if (r->crc) r->crc_table = arena_take(&arena_psram, 1024);
    bool ok = r->demod && r->ring && r->check && r->decode && r->work && r->scratch && r->job &&
              (r->ts || !r->options.ts) && (r->order || !r->options.ts) && (r->crc_table || !r->crc);
    xQueueReset(full_q);
    xQueueReset(empty_q);
    xQueueReset(jobs_q);
    xQueueReset(results_q);
    xQueueReset(done_sem);
    for (unsigned i = 0; ok && i < SLOTS; ++i) {
        r->slot[i].hard = arena_take(&arena_high, RX_HARD_BYTES);
        r->slot[i].weak = arena_take(&arena_high, RX_HARD_BYTES);
        /* Four levels for the decoder at 1 Msym/s only: the fine plane doubles the release's
         * demap cost, which core 0 can spare there (8% busy) but not at 5 Msym/s (39 to 63%),
         * where the decoder's time is the limit (results/2026-10-24-rx-fec-curve). */
        r->slot[i].fine = four ? arena_take(&arena_fine, RX_HARD_BYTES) : NULL;
        ok = r->slot[i].hard && r->slot[i].weak && (r->slot[i].fine || !four);
        uint8_t n = i;
        if (ok && i) xQueueSend(r->empty, &n, 0);   /* the release starts on slot 0 */
    }
    if (!ok) return ESP_ERR_NO_MEM;
    for (uint32_t v = 0; r->crc && v < 256; ++v) {
        uint32_t c = v;
        for (unsigned b = 0; b < 8; ++b) c = c >> 1 ^ (0xedb88320U & -(c & 1));
        r->crc_table[v] = c;
    }
    if (rx_demod_init(r->demod, &r->config, r->ring) || rx_fec_init(r->check, modcod, NULL) ||
        rx_fec_init(r->decode, modcod, r->work) || rx_fec_set_scratch(r->decode, r->scratch))
        return ESP_ERR_INVALID_ARG;
    if (r->ts) rx_ts_init(r->ts, r->options.drop_null, emit, r);
    if (r->order) rx_order_init(r->order, ORDER_FRAMES, deframe, r);
    if (r->options.mon) {
        r->options.mon->ts = r->ts;
        r->options.mon->demod = r->demod;
    }
    /* Static stacks: never in slow RTC FAST memory. */
    static StackType_t stacks[3][STACK / sizeof(StackType_t)];
    static StaticTask_t tasks[3];
    r->release = xTaskCreateStaticPinnedToCore(releaser, "release", STACK, r, 14, stacks[0], &tasks[0], 0);
    r->checker = xTaskCreateStaticPinnedToCore(checker, "check", STACK, r, 11, stacks[1], &tasks[1], 0);
    r->decoder = xTaskCreateStaticPinnedToCore(decoder, "decode", STACK, r, 5, stacks[2], &tasks[2], 0);
    if (!r->release || !r->checker || !r->decoder) return ESP_ERR_NO_MEM;
    r->stats->free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    r->stats->largest_internal = heap_caps_get_largest_free_block(INTERNAL);
    printf("@{\"cmd\":\"rx\",\"stream\":\"binary\",\"record_bytes\":%u,\"payload\":\"%s\","
           "\"failed\":\"hard+weak\",\"ts\":%s}\n", (unsigned)sizeof(receive_record_t),
           r->crc ? "crc32" : "bytes", r->ts ? "true" : "false");
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

/* Stop the core 0 tasks after the last frames and close the stream. The memory goes back to the
 * arenas when the next run resets them. */
static void finish(receiver_t *r)
{
    receive_stats_t *s = r->stats;
    const bool running = r->release && r->checker && r->decoder;
    if (r->options.mon) {           /* the sender stops tapping before the demodulator goes */
        r->options.mon->demod = NULL;
        r->options.mon->ts = NULL;
    }
    if (running) {
        r->stop = true;
        xTaskNotifyGive(r->release);
        xSemaphoreTake(r->done, portMAX_DELAY);
        uint8_t stop = STOP;
        xQueueSend(r->full, &stop, portMAX_DELAY);
        xSemaphoreTake(r->done, portMAX_DELAY);
        xQueueSend(r->jobs, &stop, portMAX_DELAY);
        xSemaphoreTake(r->done, portMAX_DELAY);
        receive_record_t end = {RECEIVE_MAGIC, 0, 0, 0, RECEIVE_END, 0, 0};
        receive_write(&end, sizeof end);
        rx_demod_stats(r->demod, &s->demod);
        if (r->ts) {
            rx_ts_stats(r->ts, &s->ts);
            while (s->ts_pids < RX_TS_PIDS && rx_ts_pid(r->ts, s->ts_pids, &s->pids[s->ts_pids]))
                ++s->ts_pids;
        }
    }
    reap(r->release, running);
    reap(r->checker, running);
    reap(r->decoder, running);
}

static void setup(receiver_t *r, unsigned modcod, unsigned rate, unsigned mhz,
                  const receive_options_t *options, receive_stats_t *s)
{
    memset(s, 0, sizeof *s);
    *r = (receiver_t){.stats = s, .crc = rate == 5000000, .options = *options,
                      .last = -1, .full = full_q, .empty = empty_q, .done = done_sem, .usb = usb_sem,
                      .jobs = jobs_q, .results = results_q, .free_jobs = (1U << JOBS) - 1,
                      .config = {.modcod = modcod, .symbol_rate = rate, .cfo_hint_hz = -10000,
                                 .cfo_span_hz = 30000, .carrier_khz = mhz * 1000U}};
}

esp_err_t receive_run(const capture_config_t *capture, unsigned modcod, unsigned rate,
                      const receive_options_t *options, receive_stats_t *s, capture_stats_t *cs)
{
    receiver_t r;
    setup(&r, modcod, rate, capture->mhz, options, s);
    capture_config_t c = *capture;
    c.ready = start;
    esp_err_t error = capture_run(&c, consume, &r, cs);
    finish(&r);
    return error;
}

esp_err_t receive_replay(const int8_t *sc8, size_t pairs, unsigned modcod, unsigned rate,
                         unsigned skip, const receive_options_t *options, receive_stats_t *s,
                         capture_stats_t *cs)
{
    receiver_t r;
    setup(&r, modcod, rate, 2420, options, s);
    capture_config_t c = {.mhz = 2420, .ready = start};
    esp_err_t error = capture_replay(&c, sc8, pairs, skip, consume, &r, cs);
    finish(&r);
    return error;
}
