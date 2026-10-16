// DVB-S2 from the chip: QPSK normal frames encoded continuously on core 0 and shaped into the DAC
// banks on core 1.
//
//   dvb <MHz> <modcod 4..11> <1..60 seconds> [0..17 gain] [16..508 peak level]
//   dvbdump <modcod 4..11> <frames> [peak level] [decimation] [stall ms]   the samples, no radio
//   dvbpace <modcod 4..11> <seconds> [peak level] [stall ms]   the timed fills, no radio
//
// An encoder task on core 0 writes frames with pilots, as phase codes 16 k, into a ring of
// RING_SLOTS frame slots in PSRAM (ring.h); the bank fill on core 1 reads the symbols it needs
// from there. Frame e carries bytes [e * B, (e + 1) * B) of one payload stream,
// B = (kbch - 80) / 8: xorshift32 seeded with 2026 (x ^= x << 13, x ^= x >> 17, x ^= x << 5;
// byte = x & 0xff), so host/onair.py can rebuild it. If the encoder is late, the fill plays a
// filler frame instead, the same MODCOD carrying an empty BBFRAME (DFL = 0), and counts an
// underrun; payload is never lost or reordered. `stall` makes the encoder sleep that long after
// each frame, to provoke underruns.
//
// One symbol is 40 DAC samples: 1 Msym/s at 40 MS/s. The pulse is the roll-off 0.35 table,
// matching the roll-off the encoder signals, scaled so the worst-case |I + jQ| is the requested
// level (default 128, the edge of the transmit chain's linear range, results/2026-10-16-radio).

#include "dvbtx.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "dvbs2.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radio_s3.h"
#include "ring.h"
#include "shaper.h"
#include "shaper_taps.h"

#define RING_SLOTS 8                // frames the encoder may run ahead: 233 ms at 1 Msym/s
#define PILOTS 1
#define SPS 40                      // samples per symbol
#define BANK_SYMBOLS 412            // a 16380-sample fill spans at most 411 symbols, plus one
#define TAPS (9 * SPS)
#define WORST_PEAK 718.4            // max |I + jQ| of the full-scale a35 table (host/shaper.py)
#define SEED 2026
#define ENCODER_PRIORITY 5          // above the command loop, below the Wi-Fi and IPC tasks

typedef struct {
    ring_t ring;
    const uint32_t *blocks;         // shaper table
    uint8_t win[SHAPER_HISTORY + BANK_SYMBOLS];
    uint32_t part[SPS] __attribute__((aligned(16)));
    uint32_t max_fill_cycles;       // one fill call
} dvb_t;

static DRAM_ATTR dvb_t dvb;

typedef struct {
    const dvbs2_mode *mode;
    dvbs2_workspace *ws;
    uint8_t *payload;               // one frame of payload, PSRAM
    uint8_t *slots, *filler;        // PSRAM
    uint32_t stall_ms;
    volatile bool stop;
    bool failed;
    SemaphoreHandle_t exited;
    uint32_t frames;                // encoded and published
    uint64_t cycles;                // per frame: payload, encoding and phase codes, raw
    uint32_t min_cycles, max_cycles;
} encoder_t;

static encoder_t enc;
static StaticSemaphore_t exited_storage;

static uint32_t next_byte(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x & 0xff;
}

// Samples [first, first + count) into dst. dst stays 16-byte aligned wherever a whole symbol
// starts: a bank holds sample n at word n % 16380, and 16380 and 40 are both multiples of 4.
static IRAM_ATTR esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    const uint32_t before = esp_cpu_get_cycle_count();
    const int nsym = ring_window(&dvb.ring, dvb.win, sizeof dvb.win, first, count, SPS,
                                 SHAPER_HISTORY, RADIO_S3_BANK_WORDS);
    if (nsym < 0) {
        return ESP_FAIL;
    }
    const uint8_t *sym = dvb.win + SHAPER_HISTORY;
    size_t skip = first % SPS;
    if (skip) {
        shaper_fill40_pie(dvb.part, sym++, 1, dvb.blocks);
        size_t take = SPS - skip < count ? SPS - skip : count;
        memcpy(dst, dvb.part + skip, take * 4);
        dst += take, count -= take;
    }
    const size_t whole = count / SPS;
    if (whole) {
        shaper_fill40_pie(dst, sym, (int)whole, dvb.blocks);
        dst += whole * SPS, count -= whole * SPS, sym += whole;
    }
    if (count) {
        shaper_fill40_pie(dvb.part, sym, 1, dvb.blocks);
        memcpy(dst, dvb.part, count * 4);
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - before;
    if (cycles > dvb.max_fill_cycles) {
        dvb.max_fill_cycles = cycles;
    }
    return ESP_OK;
}

// One PL frame of phase codes 16 k from `bytes` payload bytes (0: an empty BBFRAME).
static int encode(const dvbs2_mode *mode, dvbs2_workspace *ws, const uint8_t *payload,
                  size_t bytes, uint8_t *out, size_t symbols)
{
    if (dvbs2_bb_gcs(ws, mode, payload, bytes * 8)) {
        return -1;
    }
    dvbs2_bb_scramble(ws, mode);
    dvbs2_bch(ws, mode);
    dvbs2_ldpc(ws, mode);
    dvbs2_map(ws, mode, out + 90);
    dvbs2_plframe(mode, PILOTS, out);
    for (size_t i = 0; i < symbols; ++i) {
        out[i] <<= 4;
    }
    return 0;
}

// Core 0: frame after frame into the ring, waiting while every slot is still needed.
static void encoder_task(void *arg)
{
    encoder_t *e = arg;
    const size_t bytes = (e->mode->kbch - 80) / 8;
    uint32_t x = SEED;
    for (uint32_t f = 0; !e->stop; ++f) {
        uint8_t *out;
        while (!(out = ring_slot(&dvb.ring, f))) {
            if (e->stop) {
                goto done;
            }
            vTaskDelay(1);
        }
        const uint32_t start = esp_cpu_get_cycle_count();
        for (size_t i = 0; i < bytes; ++i) {
            e->payload[i] = next_byte(&x);
        }
        if (encode(e->mode, e->ws, e->payload, bytes, out, dvb.ring.frame_symbols)) {
            e->failed = true;
            goto done;
        }
        const uint32_t spent = esp_cpu_get_cycle_count() - start;
        ring_publish(&dvb.ring, f);
        e->frames = f + 1;
        e->cycles += spent;
        e->min_cycles = spent < e->min_cycles ? spent : e->min_cycles;
        e->max_cycles = spent > e->max_cycles ? spent : e->max_cycles;
        if (e->stall_ms) {
            vTaskDelay(pdMS_TO_TICKS(e->stall_ms));
        }
    }
done:
    xSemaphoreGive(e->exited);
    vTaskDelete(NULL);
}

static void release(void)
{
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_free(enc.ws);
    heap_caps_free(enc.payload);
    heap_caps_free(enc.slots);
    heap_caps_free(enc.filler);
    enc.ws = NULL, enc.payload = enc.slots = enc.filler = NULL;
}

// Build the scaled table and the filler frame, start the encoder on core 0 and wait until it has
// filled every slot. Returns the frame length in symbols, or 0 after printing an error for `cmd`.
static size_t start_encoder(const char *cmd, unsigned modcod, unsigned level, unsigned stall_ms)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    if (!mode || mode->bits_per_symbol != 2) {
        printf("@{\"cmd\":\"%s\",\"error\":\"QPSK modcods 4..11 only\"}\n", cmd);
        return 0;
    }
    const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
    (void)heap_caps_monitor_local_minimum_free_size_start();   // low-water mark of this command
    memset(&enc, 0, sizeof enc);
    enc.mode = mode;
    enc.stall_ms = stall_ms;
    enc.min_cycles = UINT32_MAX;
    enc.ws = heap_caps_aligned_alloc(16, sizeof *enc.ws, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    enc.payload = heap_caps_malloc((mode->kbch - 80) / 8, MALLOC_CAP_SPIRAM);
    enc.slots = heap_caps_malloc(RING_SLOTS * symbols, MALLOC_CAP_SPIRAM);
    enc.filler = heap_caps_malloc(symbols, MALLOC_CAP_SPIRAM);
    if (!enc.ws || !enc.payload || !enc.slots || !enc.filler) {
        printf("@{\"cmd\":\"%s\",\"error\":\"allocation failed\"}\n", cmd);
        release();
        return 0;
    }
    if (encode(mode, enc.ws, NULL, 0, enc.filler, symbols)) {
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder failed\"}\n", cmd);
        release();
        return 0;
    }
    // Scale the pulse so the worst case over all symbol histories peaks at `level`.
    static int32_t taps[TAPS];
    for (int i = 0; i < TAPS; ++i) {
        taps[i] = (int32_t)((double)shaper_taps_qpsk_n40_a35[i] * level / WORST_PEAK);
    }
    shaper_table_t table;
    shaper_build(&table, shaper_store, taps, SPS, 2);
    dvb.blocks = shaper_store;
    dvb.max_fill_cycles = 0;
    ring_init(&dvb.ring, enc.slots, RING_SLOTS, enc.filler, symbols);

    enc.exited = xSemaphoreCreateBinaryStatic(&exited_storage);
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", 4096, &enc, ENCODER_PRIORITY, NULL, 0)
            != pdPASS) {
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder task failed\"}\n", cmd);
        release();
        return 0;
    }
    const TickType_t limit = xTaskGetTickCount() + pdMS_TO_TICKS(RING_SLOTS * (stall_ms + 100));
    while (__atomic_load_n(&dvb.ring.published, __ATOMIC_ACQUIRE) < RING_SLOTS && !enc.failed &&
           (int32_t)(xTaskGetTickCount() - limit) < 0) {
        vTaskDelay(1);
    }
    if (__atomic_load_n(&dvb.ring.published, __ATOMIC_ACQUIRE) < RING_SLOTS) {
        enc.stop = true;
        xSemaphoreTake(enc.exited, portMAX_DELAY);
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder did not fill the ring\"}\n", cmd);
        release();
        return 0;
    }
    return symbols;
}

// Stop the encoder after a run and print the stream part of the result record.
static void stop_encoder(void)
{
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    enc.stop = true;
    xSemaphoreTake(enc.exited, portMAX_DELAY);
    const ring_t *r = &dvb.ring;
    printf(",\"ring_slots\":%d,\"stall_ms\":%" PRIu32 ",\"frames_encoded\":%" PRIu32 ","
           "\"frames_played\":%" PRIu32 ",\"positions\":%" PRIu32 ",\"underruns\":%" PRIu32 ","
           "\"min_lead_symbols\":%lld,\"encode_cycles_mean\":%" PRIu64 ","
           "\"encode_cycles_min\":%" PRIu32 ",\"encode_cycles_max\":%" PRIu32 ","
           "\"encoder_failed\":%s,\"max_fill_call_cycles\":%" PRIu32 ",\"free_internal\":%u,"
           "\"largest_internal\":%u,\"min_free_internal\":%u",
           RING_SLOTS, enc.stall_ms, enc.frames, r->next_frame, r->next_position, r->underruns,
           r->min_lead == INT64_MAX ? -1LL : (long long)r->min_lead,
           enc.frames ? enc.cycles / enc.frames : 0, enc.frames ? enc.min_cycles : 0,
           enc.max_cycles, enc.failed ? "true" : "false", dvb.max_fill_cycles,
           (unsigned)internal, (unsigned)largest, (unsigned)lowest);
    release();
}

static void print_mode(const char *cmd, unsigned modcod, size_t symbols, unsigned level)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    printf("@{\"cmd\":\"%s\",\"modcod\":%u,\"rate\":\"%s\",\"symbol_rate\":%u,\"rolloff\":0.35,"
           "\"pilots\":%d,\"frame_symbols\":%u,\"payload_bytes_per_frame\":%u,\"peak_level\":%u",
           cmd, modcod, mode->rate, RADIO_S3_DAC_RATE / SPS, PILOTS, (unsigned)symbols,
           (unsigned)((mode->kbch - 80) / 8), level);
}

static void print_timing(const radio_s3_stats_t *s)
{
    printf(",\"fills\":%" PRIu32 ",\"switches\":%" PRIu32 ",\"late_fills\":%" PRIu32 ","
           "\"late_switches\":%" PRIu32 ",\"max_fill_cycles\":%" PRIu32 ","
           "\"min_fill_slack_cycles\":%" PRIu32 ",\"max_switch_late_cycles\":%" PRIu32 ","
           "\"fatal_late\":%" PRIu32 ",\"elapsed_samples\":%" PRIu64,
           s->fills, s->switches, s->late_fills, s->late_switches, s->max_fill_cycles,
           s->min_fill_slack_cycles, s->max_switch_late_cycles, s->fatal_late,
           s->elapsed_samples);
}

void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level)
{
    const size_t symbols = start_encoder("dvb", modcod, level, 0);
    if (!symbols) {
        return;
    }
    radio_s3_stats_t s = {0};
    const esp_err_t err = radio_s3_run(freq, (uint8_t)gain, seconds, fill, NULL, &s);
    print_mode("dvb", modcod, symbols, level);
    printf(",\"freq_mhz\":%" PRIu32 ",\"seconds\":%" PRIu32 ",\"gain\":%u", freq, seconds, gain);
    print_timing(&s);
    printf(",\"fe_gain_index\":%" PRIu32, s.fe_gain_index);
    stop_encoder();
    printf(",\"status\":\"%s\"%s}\n", esp_err_to_name(err),
           err != ESP_OK ? ",\"error\":\"radio run failed\"" : "");
}

void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms)
{
    const size_t symbols = start_encoder("dvbpace", modcod, level, stall_ms);
    if (!symbols) {
        return;
    }
    radio_s3_stats_t s;
    const esp_err_t err = radio_s3_paced_run(seconds, fill, NULL, &s);
    print_mode("dvbpace", modcod, symbols, level);
    printf(",\"seconds\":%" PRIu32 ",\"step_words\":%u", seconds, RADIO_S3_STEP_WORDS);
    print_timing(&s);
    stop_encoder();
    printf(",\"status\":\"%s\"%s}\n", esp_err_to_name(err),
           err != ESP_OK ? ",\"error\":\"paced run failed\"" : "");
}

typedef struct {
    unsigned decim;
    size_t used;
    uint32_t crc;
    uint8_t buf[4096];
} dump_t;

static void flush(dump_t *d)
{
    d->crc = esp_rom_crc32_le(d->crc, d->buf, d->used);
    usb_serial_jtag_write_bytes(d->buf, d->used, portMAX_DELAY);
    d->used = 0;
}

// Every decim-th played sample as little-endian int16 I, Q (sign-extended 10-bit fields).
static esp_err_t sink(const uint32_t *bank, uint64_t first, size_t count, void *ctx)
{
    dump_t *d = ctx;
    for (uint64_t n = (first + d->decim - 1) / d->decim * d->decim; n < first + count;
         n += d->decim) {
        const uint32_t w = bank[n % RADIO_S3_BANK_WORDS];
        const int16_t iq[2] = {(int16_t)((int32_t)(w << 22) >> 22),
                               (int16_t)((int32_t)(w << 12) >> 22)};
        memcpy(d->buf + d->used, iq, 4);
        d->used += 4;
        if (d->used == sizeof d->buf) {
            flush(d);
        }
    }
    return ESP_OK;
}

void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                unsigned stall_ms)
{
    const size_t symbols = start_encoder("dvbdump", modcod, level, stall_ms);
    if (!symbols) {
        return;
    }
    const uint64_t samples = (uint64_t)frames * symbols * SPS;
    const uint64_t bytes = (samples + decim - 1) / decim * 4;
    static dump_t d;
    d.decim = decim, d.used = 0, d.crc = 0;
    printf("@{\"cmd\":\"dvbdump\",\"binary_bytes\":%" PRIu64 ",\"format\":\"int16 I/Q LE\","
           "\"sample_rate\":%u}\n", bytes, RADIO_S3_DAC_RATE / decim);
    fflush(stdout);
    radio_s3_dry_stats_t s;
    const esp_err_t err = radio_s3_dry_run(fill, NULL, samples, sink, &d, &s);
    if (d.used) {
        flush(&d);
    }
    print_mode("dvbdump", modcod, symbols, level);
    printf(",\"frames\":%u,\"decimation\":%u,\"samples\":%" PRIu64 ",\"crc32\":%" PRIu32 ","
           "\"fills\":%" PRIu32 ",\"max_fill_cycles\":%" PRIu32 ",\"overlap_words\":%" PRIu32 ","
           "\"overlap_mismatches\":%" PRIu32,
           frames, decim, samples, d.crc, s.fills, s.max_fill_cycles, s.overlap_words,
           s.overlap_mismatches);
    stop_encoder();
    printf(",\"status\":\"%s\"}\n", esp_err_to_name(err));
}
