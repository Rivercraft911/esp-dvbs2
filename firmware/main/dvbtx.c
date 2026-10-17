// Continuous QPSK at 1 Msym/s (40 sps), or 8PSK at 5/10 Msym/s (8/4 sps).
// 8PSK requires an explicit 5m or 10m before duration/frame count in all commands.
// Core 0 encodes raw phases into PSRAM, core 1 copies windows and shapes DAC banks.
// Payload: xorshift32 seed 2026, one contiguous byte stream across data frames.
// On underrun the ring inserts a 3330-symbol dummy PLFRAME (EN 302 307-1 5.1.5,
// 5.5.1), preserving each choice for overlapping bank reads. No payload is skipped.
// RRC roll-off 0.35 matches the BBHEADER (dvbs2.h). Peak scaling uses the
// conservative bound sqrt(2)*508 LSB for every table (host/shaper.py Profile.taps).
// The taps divide out the DAC's sample hold (host/shaper.py corrected_rrc) because the
// radio sends the held signal; a raw dvbdump has no hold and measures differently.

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
#define BANK_SYMBOLS 4096           // ceil(16380/4) + 1, worst offset at 4 sps
#define TAPS (9 * 40)
#define WORST_PEAK 718.4            // conservative sqrt(2)*508 LSB axis bound, before rounding
#define SEED 2026
#define ENCODER_PRIORITY 5          // above the command loop, below the Wi-Fi and IPC tasks

typedef struct {
    ring_t ring;
    shaper_table_t table;
    uint8_t win[SHAPER_HISTORY + BANK_SYMBOLS];
    uint32_t max_fill_cycles;       // one fill call
} dvb_t;

static DRAM_ATTR dvb_t dvb;

typedef struct {
    const dvbs2_mode *mode;
    dvbs2_fast *fast;               // fast encoder context for this mode, internal RAM
    dvbs2_workspace *ws;            // cached PSRAM
    uint8_t *payload;               // one frame of payload, PSRAM
    uint8_t *slots, *filler;        // PSRAM
    uint32_t stall_ms;
    volatile bool stop;
    bool failed;
    SemaphoreHandle_t exited;
    uint32_t frames;                // encoded and published
    uint64_t cycles;                // per frame: payload and encoding into PSRAM, raw
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

// A whole-symbol start n has n % sps == 0. Since sps = 4, 8 or 40 and
// 16380 are multiples of four, (n % 16380)*4 bytes is 16-byte aligned.
// At 8 sps 16380 % 8 == 4, so a bank wrap can split a symbol in half.
static IRAM_ATTR esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    const uint32_t before = esp_cpu_get_cycle_count();
    const unsigned sps = dvb.table.n;
    if (ring_window(&dvb.ring, dvb.win, sizeof dvb.win, first, count, sps,
                    SHAPER_HISTORY, RADIO_S3_BANK_WORDS) < 0) {
        return ESP_FAIL;
    }
    shaper_samples(dst, dvb.win + SHAPER_HISTORY, first % sps, count, &dvb.table);
    const uint32_t cycles = esp_cpu_get_cycle_count() - before;
    if (cycles > dvb.max_fill_cycles) {
        dvb.max_fill_cycles = cycles;
    }
    return ESP_OK;
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
        if (dvbs2_encode_frame(e->fast, e->ws, NULL, e->payload, bytes * 8, out)) {
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
    heap_caps_free(enc.fast);
    heap_caps_free(enc.ws);
    heap_caps_free(enc.payload);
    heap_caps_free(enc.slots);
    heap_caps_free(enc.filler);
    enc.fast = NULL, enc.ws = NULL, enc.payload = enc.slots = enc.filler = NULL;
}

// Build the scaled table and the dummy frame, start the encoder on core 0 and wait until it has
// filled every slot. Returns the frame length in symbols, or 0 after printing an error for `cmd`.
static size_t start_encoder(const char *cmd, unsigned modcod, unsigned level, unsigned stall_ms,
                            unsigned sps)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    if (!mode || (mode->bits_per_symbol == 2 ? sps != 40 : (sps != 8 && sps != 4))) {
        printf("@{\"cmd\":\"%s\",\"error\":\"invalid MODCOD/sps\"}\n", cmd);
        return 0;
    }
    const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
    (void)heap_caps_monitor_local_minimum_free_size_start();   // low-water mark of this command
    memset(&enc, 0, sizeof enc);
    enc.mode = mode;
    enc.stall_ms = stall_ms;
    enc.min_cycles = UINT32_MAX;
    // Fast encoder: per-mode context in internal RAM, workspace in cached PSRAM
    // (components/dvbs2/include/dvbs2.h).
    enc.fast = heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    enc.ws = heap_caps_aligned_alloc(16, sizeof *enc.ws, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    enc.payload = heap_caps_malloc((mode->kbch - 80) / 8, MALLOC_CAP_SPIRAM);
    enc.slots = heap_caps_malloc(RING_SLOTS * symbols, MALLOC_CAP_SPIRAM);
    enc.filler = heap_caps_malloc(DVBS2_DUMMY_SYMBOLS, MALLOC_CAP_SPIRAM);
    if (!enc.fast || !enc.ws || !enc.payload || !enc.slots || !enc.filler ||
            dvbs2_fast_prepare(enc.fast, mode, PILOTS, 15)) {
        printf("@{\"cmd\":\"%s\",\"error\":\"allocation failed\"}\n", cmd);
        release();
        return 0;
    }
    dvbs2_dummy(enc.filler);
    // Scale the conservative envelope to level; table rounding adds at most 1.5 LSB/axis.
    static int32_t taps[TAPS];
    const int32_t *source = sps == 40 ? shaper_taps_qpsk_n40_a35 :
                            sps == 8 ? shaper_taps_8psk_n8_a35 : shaper_taps_8psk_n4_a35;
    for (unsigned i = 0; i < 9 * sps; ++i) {
        taps[i] = (int32_t)((double)source[i] * level / WORST_PEAK);
    }
    shaper_build(&dvb.table, shaper_store, taps, sps, mode->bits_per_symbol);
    dvb.max_fill_cycles = 0;
    ring_init(&dvb.ring, enc.slots, RING_SLOTS, enc.filler, symbols);
    ring_filler_length(&dvb.ring, DVBS2_DUMMY_SYMBOLS);

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
           cmd, modcod, mode->rate, RADIO_S3_DAC_RATE / dvb.table.n, PILOTS, (unsigned)symbols,
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

void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps)
{
    const size_t symbols = start_encoder("dvb", modcod, level, 0, sps);
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

void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps)
{
    const size_t symbols = start_encoder("dvbpace", modcod, level, stall_ms, sps);
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
                unsigned stall_ms, unsigned sps)
{
    const size_t symbols = start_encoder("dvbdump", modcod, level, stall_ms, sps);
    if (!symbols) {
        return;
    }
    const uint64_t samples = (uint64_t)frames * symbols * dvb.table.n;
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
