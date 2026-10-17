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
        if (enc.quads) {
            uint8_t blocks[1400] = {0};
            const unsigned row = 4*sps, skip = at % row;
            const int64_t first_symbol = (int64_t)(at/row)*4 - 8;
            const size_t need = 8 + 4*((skip+count+row-1)/row);
            for (size_t i = 0; i < need; ++i) {
                const int64_t k = first_symbol+i;
                unsigned code = k < 0 ? 0 : phases[k] >> 1;
                blocks[i/4] |= code << (6-2*(i&3));
            }
            shaper_qpsk_c(reference, blocks+2, (skip+count+row-1)/row, dvb.table.blocks, sps);
            for (size_t i = 0; i < count; ++i)
                assert(banks[b][(at+i)%RADIO_S3_BANK_WORDS] == reference[skip+i]);
            at += count;
            continue;
        }
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
    if (dvb.ring.packed) {
        assert(dvb.shaped_words == dvb.sample_end);
        assert(dvb.sample_end == first + RADIO_S3_BANK_WORDS);
    }
    stats->fills++;
}

// The bank sequence of radio_s3.c's dry_worker at the rate's geometry (radio_s3.h).
esp_err_t radio_s3_dry_run_rate(radio_s3_fill_fn fn, void *ctx, uint64_t samples,
                                uint32_t rate, radio_s3_sink_fn sink_fn, void *sink_ctx,
                                radio_s3_dry_stats_t *stats)
{
    const uint64_t step = RADIO_S3_STEP_FOR(rate), lead = RADIO_S3_LEAD_FOR(rate);
    *stats = (radio_s3_dry_stats_t){0};
    bank_fill(fn, ctx, 0, 0, stats);
    bank_fill(fn, ctx, 1, step - lead, stats);
    for (uint64_t k = 0; k * step < samples; ++k) {
        const uint64_t first = k * step;
        const size_t count = samples - first < step ? (size_t)(samples - first) : (size_t)step;
        assert(sink_fn(banks[k & 1], first, count, sink_ctx) == ESP_OK);
        if (first + step < samples)
            bank_fill(fn, ctx, k & 1, (k + 2) * step - lead, stats);
    }
    assert(dvb.ring.underruns == 0);
    return ESP_OK;
}

esp_err_t radio_s3_paced_run_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn, void *ctx,
                                  radio_s3_stats_t *stats)
{ (void)sec; (void)rate; (void)fn; (void)ctx; (void)stats; abort(); }
esp_err_t radio_s3_run_bandwidth(uint32_t freq, uint8_t gain, uint32_t sec, uint32_t rate,
                                 uint32_t bandwidth, radio_s3_fill_fn fn, void *ctx, radio_s3_stats_t *stats)
{ (void)freq; (void)gain; (void)sec; (void)rate; (void)bandwidth; (void)fn; (void)ctx; (void)stats; abort(); }
void radio_filter_print(const radio_filter_stats_t *stats) { (void)stats; }
static unsigned wifi_probes;
esp_err_t radio_s3_wifi_probe(radio_s3_stats_t *stats)
{
    assert(enc.fast && enc.ws && enc.payload && dvb.win);
    memset(stats, 0, sizeof *stats);
    ++wifi_probes;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5 || argc == 6 || argc == 7 || argc == 8);
    if (argc >= 5) callsign = argv[4];
    const uint32_t rate = argc >= 6 ? (uint32_t)atol(argv[5]) : RADIO_S3_DAC_RATE;
    const unsigned mc = (unsigned)atoi(argv[1]), sps = (unsigned)atoi(argv[2]), frames = argc == 8 ? (unsigned)atoi(argv[7]) : 40;
    const unsigned pulse = argc >= 7 ? atoi(argv[6]) : 0;
    const bool quads = pulse == 2 || sps == 3, short_pulse = pulse == 1;
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
    // Every command-owned allocation can fail: count them on a good start, then fail each.
    // Cleanup must leave no live allocations.
    allocation_count = 0;
    assert(start_encoder("dvbdump", mc, 128, 0, sps, rate, pulse));
    const int allocations = allocation_count;
    const bool resident = sps != 40 && (mc < 12 || sps == 4);
    assert(allocations == (sps == 40 ? 7 : 3 + (mc >= 12 && !short_pulse) +
                          (resident && mode->t > 8) + !resident));
    for (unsigned i = 0; i < 64; ++i) {
        if (native_blocks[i].p == enc.ws)
            assert(native_blocks[i].caps == ((resident ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT));
        if (enc.bch && native_blocks[i].p == enc.bch)
            assert(native_blocks[i].n == DVBS2_BCH_CACHE_BYTES && (native_blocks[i].caps & MALLOC_CAP_INTERNAL));
    }
    if (quads && sps == 4) assert((uint32_t *)enc.ws == shaper_store+QPSK_TABLE_WORDS(4));
    assert(!((uintptr_t)enc.fast & 15) && !((uintptr_t)enc.ws & 15));
    if (resident) assert(dvb.win == (uint8_t *)enc.ws + DVBS2_FAST_WORKSPACE_USED);
    assert(dvb.win_bytes <= (sps == 3 ? 1370u : sps == 4 ? 4105u : sps == 8 ? 2058u : 420u));
    stop_encoder();
    printf("}\n");
    assert(live_allocations == 0);
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder("dvbdump", mc, 128, 0, sps, rate, pulse));
        assert(live_allocations == 0);
    }
    fail_after = -1;
    if (*callsign) {
        dvbtx_wifi(mc, sps, rate, pulse);
        assert(wifi_probes == 1 && live_allocations == 0);
        assert((dvb.table.parts == 2) == short_pulse);
    }
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        capture = fopen(argv[3], "wb"); assert(capture);
        dvbtx_dump(mc, frames, 128, sps == 40 ? 10 : mc < 12 && sps == 8 ? 2 : 1, 0, sps, rate, pulse);
        fclose(capture);
        assert(live_allocations == 0);
        for (unsigned i = 0; i < 4; ++i) assert(native_ranges[i] == 0);
    }
    free(phases);
}
