// Actual dvbtx.c lifecycle with pthread task/notification stubs, no peripherals.
// Timing is synthetic. Compare every filled DAC word against the plain encoder.
#include "native.h"
#include "dvbtx.c"

static uint32_t banks[2][RADIO_S3_BANK_WORDS] __attribute__((aligned(16)));
static uint32_t reference[16440] __attribute__((aligned(16)));
static uint8_t *phases;
static size_t phase_count;
static FILE *capture;
static const char *callsign = "";

// Station store and command gate: the stored callsign without NVS.
const char *station_store_get(void) { return callsign; }
bool station_require(const char *cmd) { (void)cmd; return callsign[0] != 0; }

int usb_serial_jtag_write_bytes(const void *buf, size_t n, unsigned timeout)
{
    (void)timeout;
    assert(fwrite(buf, 1, n, capture) == n);
    return (int)n;
}

static void bank_fill(radio_s3_fill_fn fn, void *ctx, unsigned b, uint64_t first,
                       radio_s3_dry_stats_t *stats)
{
    const size_t offset = first % RADIO_S3_BANK_WORDS;
    const size_t parts[] = {RADIO_S3_BANK_WORDS - offset, offset};
    uint64_t at = first;
    for (unsigned p = 0; p < 2; ++p) {
        const size_t count = parts[p];
        if (!count) continue;
        const unsigned sps = dvb.table.n;
        const size_t end_symbol = (at + count + sps - 1) / sps;
        const size_t needed = (end_symbol + dvb.ring.frame_symbols - 1) / dvb.ring.frame_symbols;
        // Host correctness test only: wait for the native encoder, with a deadlock timeout.
        unsigned tries = 0;
        while (__atomic_load_n(&dvb.ring.published, __ATOMIC_ACQUIRE) < needed) {
            assert(tries++ < 5000);
            vTaskDelay(1);
        }
        assert(fn(banks[b] + at % RADIO_S3_BANK_WORDS, at, count, ctx) == ESP_OK);
        uint8_t shifted[SHAPER_HISTORY + BANK_SYMBOLS];
        const int64_t s0 = (int64_t)(at / sps) - SHAPER_HISTORY;
        const size_t symbols = SHAPER_HISTORY + end_symbol - at / sps;
        assert(end_symbol <= phase_count);
        for (size_t i = 0; i < symbols; ++i)
            shifted[i] = s0 + (int64_t)i < 0 ? 0 : phases[(size_t)(s0 + (int64_t)i)] << 4;
        shaper_fill_c(reference, shifted + SHAPER_HISTORY,
                       (int)(symbols - SHAPER_HISTORY), &dvb.table);
        for (size_t i = 0; i < count; ++i)
            assert(banks[b][(at + i) % RADIO_S3_BANK_WORDS] == reference[at % sps + i]);
        at += count;
    }
    stats->fills++;
}

esp_err_t radio_s3_dry_run(radio_s3_fill_fn fn, void *ctx, uint64_t samples,
                           radio_s3_sink_fn sink_fn, void *sink_ctx, radio_s3_dry_stats_t *stats)
{
    *stats = (radio_s3_dry_stats_t){0};
    bank_fill(fn, ctx, 0, 0, stats);
    bank_fill(fn, ctx, 1, RADIO_S3_STEP_WORDS - RADIO_S3_LEAD_WORDS, stats);
    for (uint64_t k = 0; k * RADIO_S3_STEP_WORDS < samples; ++k) {
        const uint64_t first = k * RADIO_S3_STEP_WORDS;
        const size_t count = samples - first < RADIO_S3_STEP_WORDS ?
                             (size_t)(samples - first) : RADIO_S3_STEP_WORDS;
        assert(sink_fn(banks[k & 1], first, count, sink_ctx) == ESP_OK);
        if (first + RADIO_S3_STEP_WORDS < samples)
            bank_fill(fn, ctx, k & 1, (k + 2) * RADIO_S3_STEP_WORDS - RADIO_S3_LEAD_WORDS, stats);
    }
    assert(dvb.ring.underruns == 0);
    return ESP_OK;
}

esp_err_t radio_s3_paced_run(uint32_t sec, radio_s3_fill_fn fn, void *ctx, radio_s3_stats_t *stats)
{ (void)sec; (void)fn; (void)ctx; (void)stats; abort(); }
esp_err_t radio_s3_run(uint32_t freq, uint8_t gain, uint32_t sec, radio_s3_fill_fn fn,
                       void *ctx, radio_s3_stats_t *stats)
{ (void)freq; (void)gain; (void)sec; (void)fn; (void)ctx; (void)stats; abort(); }

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    if (argc == 5) callsign = argv[4];
    const unsigned mc = (unsigned)atoi(argv[1]), sps = (unsigned)atoi(argv[2]), frames = 16;
    const dvbs2_mode *mode = dvbs2_find_mode(mc);
    const size_t length = dvbs2_frame_symbols(mode, 1), bytes = (mode->kbch - 80) / 8;
    phase_count = (frames + 2) * length;
    phases = malloc(phase_count);
    dvbs2_workspace ws;
    uint8_t payload[8100];
    uint32_t x = SEED;
    for (unsigned f = 0; f < frames + 2; ++f) {
        for (size_t i = 0; i < bytes; ++i) payload[i] = next_byte(&x);
        station_payload_header(payload, bytes, callsign);
        assert(!dvbs2_bb_gcs(&ws, mode, payload, bytes * 8));
        dvbs2_bb_scramble(&ws, mode); dvbs2_bch(&ws, mode); dvbs2_ldpc(&ws, mode);
        uint8_t *out = phases + f * length;
        dvbs2_map(&ws, mode, out + 90); dvbs2_plframe(mode, 1, out);
    }
    // Every command-owned allocation can fail. Cleanup must leave no live allocations.
    for (int i = 0; i < (mc == 17 ? 4 : 3); ++i) {
        if (sps != 4) break;
        fail_after = i;
        assert(!start_encoder("dvbdump", mc, 128, 0, sps));
        assert(live_allocations == 0);
    }
    fail_after = -1;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        capture = fopen(argv[3], "wb"); assert(capture);
        dvbtx_dump(mc, frames, 128, sps == 40 ? 10 : 1, 0, sps);
        fclose(capture);
        assert(live_allocations == 0);
        for (unsigned i = 0; i < 4; ++i) assert(native_ranges[i] == 0);
    }
    free(phases);
}
