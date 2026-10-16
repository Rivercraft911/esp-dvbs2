// DVB-S2 on the air: QPSK normal frames encoded on the chip, shaped into the DAC banks.
//
//   dvb <MHz> <modcod 4..11> <1..60 seconds> [0..17 gain] [16..508 peak level]
//   dvbdump <modcod 4..11> <frames> [peak level] [decimation]   the same samples, no radio
//
// The command encodes FRAMES frames with pilots into PSRAM, then loops them. Payload bytes come
// from xorshift32 seeded with 2026 (x ^= x << 13, x ^= x >> 17, x ^= x << 5; byte = x & 0xff),
// continued across frames, so host/onair.py can rebuild the expected stream. One symbol is 40
// DAC samples: 1 Msym/s at 40 MS/s. The pulse is the roll-off 0.35 table, matching the roll-off
// the encoder signals, scaled so the worst-case |I + jQ| is the requested level (default 128,
// the edge of the transmit chain's linear range, results/2026-10-16-radio).

#include "dvbtx.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "dvbs2.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "radio_s3.h"
#include "shaper.h"
#include "shaper_taps.h"

#define FRAMES 8
#define PILOTS 1
#define SPS 40                      // samples per symbol
#define BANK_SYMBOLS 412            // a 16380-sample fill spans at most 411 symbols, plus one
#define TAPS (9 * SPS)
#define WORST_PEAK 718.4            // max |I + jQ| of the full-scale a35 table (host/shaper.py)

typedef struct {
    const uint8_t *ring;            // FRAMES frames of phase codes 16 k, in PSRAM
    size_t ring_symbols;
    const uint32_t *blocks;         // shaper table
    uint8_t win[SHAPER_HISTORY + BANK_SYMBOLS];
    uint32_t part[SPS] __attribute__((aligned(16)));
    uint32_t max_fill_cycles;
} dvb_t;

static DRAM_ATTR dvb_t dvb;

static uint32_t next_byte(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x & 0xff;
}

// Copy count ring symbols starting at absolute index first (wrapping) into dst.
static IRAM_ATTR void ring_copy(uint8_t *dst, uint64_t first, size_t count)
{
    size_t at = first % dvb.ring_symbols;
    while (count) {
        size_t part = dvb.ring_symbols - at;
        if (part > count) {
            part = count;
        }
        memcpy(dst, dvb.ring + at, part);
        dst += part, count -= part, at = 0;
    }
}

// Samples [first, first + count) into dst. dst stays 16-byte aligned wherever a whole symbol
// starts: a bank holds sample n at word n % 16380, and 16380 and 40 are both multiples of 4.
static IRAM_ATTR esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    const uint32_t before = esp_cpu_get_cycle_count();
    const uint64_t s0 = first / SPS;
    const size_t nsym = (size_t)((first + count + SPS - 1) / SPS - s0);
    if (nsym > BANK_SYMBOLS) {
        return ESP_ERR_INVALID_SIZE;
    }
    // History symbols before s0 come from the end of the loop at the very start.
    ring_copy(dvb.win, s0 + dvb.ring_symbols - SHAPER_HISTORY, SHAPER_HISTORY + nsym);
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

// Encode FRAMES frames into ring (phase codes 16 k). Returns the cycles spent, or 0 on error.
static uint32_t encode_ring(const dvbs2_mode *mode, uint8_t *ring, size_t frame_symbols)
{
    const size_t bytes = (mode->kbch - 80) / 8;
    dvbs2_workspace *ws = heap_caps_malloc(sizeof *ws, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *payload = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t spent = 0;
    if (ws && payload) {
        uint32_t x = 2026;
        const uint32_t start = esp_cpu_get_cycle_count();
        for (int f = 0; f < FRAMES; ++f) {
            for (size_t i = 0; i < bytes; ++i) {
                payload[i] = next_byte(&x);
            }
            uint8_t *out = ring + f * frame_symbols;
            if (dvbs2_bb_gcs(ws, mode, payload, bytes * 8)) {
                goto done;
            }
            dvbs2_bb_scramble(ws, mode);
            dvbs2_bch(ws, mode);
            dvbs2_ldpc(ws, mode);
            dvbs2_map(ws, mode, out + 90);
            dvbs2_plframe(mode, PILOTS, out);
            for (size_t i = 0; i < frame_symbols; ++i) {
                out[i] <<= 4;
            }
        }
        spent = esp_cpu_get_cycle_count() - start;
    }
done:
    heap_caps_free(ws);
    heap_caps_free(payload);
    return spent;
}

// Encode the frame loop into PSRAM and build the scaled table. Returns the frame length in
// symbols, or 0 after printing an error for `cmd`.
static size_t prepare(const char *cmd, unsigned modcod, unsigned level, uint32_t *encode_cycles)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    if (!mode || mode->bits_per_symbol != 2) {
        printf("@{\"cmd\":\"%s\",\"error\":\"QPSK modcods 4..11 only\"}\n", cmd);
        return 0;
    }
    const size_t frame_symbols = dvbs2_frame_symbols(mode, PILOTS);
    uint8_t *ring = heap_caps_malloc(FRAMES * frame_symbols, MALLOC_CAP_SPIRAM);
    if (!ring) {
        printf("@{\"cmd\":\"%s\",\"error\":\"PSRAM allocation failed\"}\n", cmd);
        return 0;
    }
    *encode_cycles = encode_ring(mode, ring, frame_symbols);
    if (!*encode_cycles) {
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder failed\"}\n", cmd);
        heap_caps_free(ring);
        return 0;
    }
    // Scale the pulse so the worst case over all symbol histories peaks at `level`.
    static int32_t taps[TAPS];
    for (int i = 0; i < TAPS; ++i) {
        taps[i] = (int32_t)((double)shaper_taps_qpsk_n40_a35[i] * level / WORST_PEAK);
    }
    shaper_table_t table;
    shaper_build(&table, shaper_store, taps, SPS, 2);
    dvb.ring = ring;
    dvb.ring_symbols = FRAMES * frame_symbols;
    dvb.blocks = shaper_store;
    dvb.max_fill_cycles = 0;
    return frame_symbols;
}

void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level)
{
    uint32_t encode_cycles;
    const size_t frame_symbols = prepare("dvb", modcod, level, &encode_cycles);
    if (!frame_symbols) {
        return;
    }
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    radio_s3_stats_t s = {0};
    const esp_err_t err = radio_s3_run(freq, (uint8_t)gain, seconds, fill, NULL, &s);
    heap_caps_free((void *)dvb.ring);

    printf("@{\"cmd\":\"dvb\",\"freq_mhz\":%" PRIu32 ",\"modcod\":%u,\"rate\":\"%s\","
           "\"symbol_rate\":%u,\"rolloff\":0.35,\"pilots\":%d,\"frames\":%d,"
           "\"frame_symbols\":%u,\"payload_bytes_per_frame\":%u,\"seconds\":%" PRIu32 ","
           "\"gain\":%u,\"peak_level\":%u,\"encode_cycles\":%" PRIu32 ",\"fills\":%" PRIu32 ","
           "\"switches\":%" PRIu32 ",\"late_fills\":%" PRIu32 ",\"late_switches\":%" PRIu32 ","
           "\"max_fill_cycles\":%" PRIu32 ",\"min_fill_slack_cycles\":%" PRIu32 ","
           "\"fatal_late\":%" PRIu32 ",\"fe_gain_index\":%" PRIu32 ",\"status\":\"%s\"",
           freq, modcod, mode->rate, RADIO_S3_DAC_RATE / SPS, PILOTS, FRAMES,
           (unsigned)frame_symbols, (unsigned)((mode->kbch - 80) / 8), seconds, gain, level,
           encode_cycles, s.fills, s.switches, s.late_fills, s.late_switches, dvb.max_fill_cycles,
           s.min_fill_slack_cycles, s.fatal_late, s.fe_gain_index, esp_err_to_name(err));
    if (err != ESP_OK) {
        printf(",\"error\":\"radio run failed\"");
    }
    printf("}\n");
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

void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim)
{
    uint32_t encode_cycles;
    const size_t frame_symbols = prepare("dvbdump", modcod, level, &encode_cycles);
    if (!frame_symbols) {
        return;
    }
    const uint64_t samples = (uint64_t)frames * frame_symbols * SPS;
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
    heap_caps_free((void *)dvb.ring);
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    printf("@{\"cmd\":\"dvbdump\",\"modcod\":%u,\"rate\":\"%s\",\"symbol_rate\":%u,"
           "\"rolloff\":0.35,\"pilots\":%d,\"loop_frames\":%d,\"frames\":%u,"
           "\"frame_symbols\":%u,\"payload_bytes_per_frame\":%u,\"peak_level\":%u,"
           "\"decimation\":%u,\"samples\":%" PRIu64 ",\"crc32\":%" PRIu32 ","
           "\"encode_cycles\":%" PRIu32 ",\"fills\":%" PRIu32 ",\"max_fill_cycles\":%" PRIu32 ","
           "\"overlap_words\":%" PRIu32 ",\"overlap_mismatches\":%" PRIu32 ",\"status\":\"%s\"}\n",
           modcod, mode->rate, RADIO_S3_DAC_RATE / SPS, PILOTS, FRAMES, frames,
           (unsigned)frame_symbols, (unsigned)((mode->kbch - 80) / 8), level, decim, samples, d.crc,
           encode_cycles, s.fills, dvb.max_fill_cycles, s.overlap_words, s.overlap_mismatches,
           esp_err_to_name(err));
}
