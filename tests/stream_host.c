// Actual dvbtx.c lifecycle with pthread task/notification stubs, no peripherals.
// Timing is synthetic. Compare every filled DAC word against the plain encoder.
#include "native.h"
#include "ring.h"
static int checked_window(ring_t *, uint8_t *, size_t, uint64_t, size_t, unsigned, unsigned, unsigned);
static int checked_task(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
static uint32_t checked_load(const volatile uint32_t *, int);
#define __atomic_load_n checked_load
#define xTaskCreatePinnedToCore checked_task
#define ring_window checked_window
#include "dvbtx.c"
#undef ring_window
#undef xTaskCreatePinnedToCore
#undef __atomic_load_n

static uint32_t checked_load(const volatile uint32_t *p, int order)
{
    static unsigned forced_mask;
    static bool forced_admission;
    if (p == &dvb.produced && !current && forced_admission) {
        forced_admission = false;
        return dvb.consumed;
    }
    if (p == &lofty_mux.published && !current && !getenv("UNPACED_LOFTY")) {
        // Host-only producer pacing at admission, without changing physical
        // bank callbacks. The hardware consumer never waits here.
        if (!lofty_mux.output) forced_mask = 0;
        const char *frame = getenv("FORCE_DUMMY_FRAME");
        unsigned bit = frame ? lofty_mux.frames == strtoul(frame,NULL,0) :
                       lofty_mux.frames == 0 ? 1 : lofty_mux.frames == 1 ? 2 : lofty_mux.frames == 5 ? 4 : 0;
        if ((frame || getenv("FORCE_DUMMIES")) && bit && !(forced_mask&bit)) {
            forced_mask |= bit;
            forced_admission = true;
            return lofty_mux.frames; // force a not-ready admission snapshot
        }
        moon_wake();
        unsigned wait = 0;
        while (!((lofty_mux.startup+lofty_mux.steady)&1) &&
               __atomic_load_n(p,order) == lofty_mux.frames) {
            assert(++wait < 5000); vTaskDelay(1);
        }
    }
    return __atomic_load_n(p,order);
}

static int checked_task(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                        unsigned priority, TaskHandle_t *task, int core)
{
    // Cross the 32-bit sequence wrap in every native queue lifecycle. Physical
    // page cursors and the 64-bit symbol timeline must remain continuous.
    if (dvb.lofty) {
        assert(priority == MOON_PRIORITY);
#ifdef QUEUE_TEST_PAGES
        assert(QUEUE_TEST_PAGES <= dvb.queue_pages);
        dvb.queue_pages = QUEUE_TEST_PAGES;
        lofty_mux.resident_limit = dvb.queue_pages < LOFTY_STORED ? dvb.queue_pages : LOFTY_STORED;
#endif
    }
    if (dvb.moon) dvb.produced = dvb.consumed = UINT32_MAX-15;
    return xTaskCreatePinnedToCore(fn,name,stack,arg,priority,task,core);
}

static int checked_window(ring_t *r, uint8_t *dst, size_t room, uint64_t first,
                           size_t count, unsigned sps, unsigned history, unsigned bank)
{
    // The nibble unpacker can reach its aligned PIE loop only when output
    // address and source symbol have equal parity. CRCs alone miss this fallback.
    if (r->packed && r->packed != 4)
        assert(!(((uintptr_t)dst ^ (first/sps-history)) & 1));
    return ring_window(r,dst,room,first,count,sps,history,bank);
}
#include "ingress_pattern.h"

// Match the physical bank alignment, including its four unused trailing words.
static uint32_t (*banks)[16384];
static unsigned bank_shift;
static uint32_t reference[16440] __attribute__((aligned(16)));
static uint8_t *phases;
static size_t phase_count;
static FILE *capture;
static const char *callsign = "";

// Real portable ingress ring, with the peripheral replaced by the GDMA host model.
extern void *sim_new(void);
extern void sim_free(void *);
extern ingress_ring *sim_ring(void *);
extern uint32_t sim_transfer(void *, const uint8_t *, uint32_t);
static void *input_sim;
static unsigned native_input, input_seq, input_reads, input_zero_reads;
static volatile bool input_empty;
static bool native_replay;
static uint8_t *native_buffers[INGRESS_SLOTS];
extern void sim_set_buffer(void *, unsigned, uint8_t *);
extern void sim_replay(void *, unsigned);
void spi_input_buffers(uint8_t *tail, size_t n, uint8_t *ws, size_t m) {
    // Borrowed DMA slots must fit and stay disjoint from every live encoder region.
    memset(native_buffers, 0, sizeof native_buffers);
    if (!native_input) return;
    assert(!((uintptr_t)tail & 3) && !((uintptr_t)ws & 3));
    if (tail) assert(tail >= (uint8_t *)shaper_store && tail+n <= (uint8_t *)(shaper_store+SHAPER_STORE_WORDS));
    unsigned count = 0;
    for (size_t at = 0; tail && at+INGRESS_SLOT_BYTES <= n && count < INGRESS_SLOTS; at += INGRESS_SLOT_BYTES)
        native_buffers[count++] = tail+at;
    for (size_t at = 0; ws && at+INGRESS_SLOT_BYTES <= m && count < INGRESS_SLOTS; at += INGRESS_SLOT_BYTES)
        native_buffers[count++] = ws+at;
    if (count < INGRESS_SLOTS) memset(native_buffers, 0, sizeof native_buffers);
    if (ws) assert(ws >= (uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED+dvb.win_bytes && ws+m <= (uint8_t *)(enc.ws+1));
}
unsigned spi_input_type(void) { return native_input; }
esp_err_t spi_input_start(void) {
    if (native_input) {
        input_sim = sim_new(); sim_ring(input_sim)->type = native_input;
        if (native_buffers[0])
            for (unsigned i = 0; i < INGRESS_SLOTS; ++i) sim_set_buffer(input_sim, i, native_buffers[i]);
        if (native_replay) sim_replay(input_sim, native_input);
        input_seq = input_reads = input_zero_reads = 0;
    }
    return ESP_OK;
}
size_t spi_input_read(uint8_t *dst, size_t n) {
    if (input_empty || (++input_reads % 7) == 0) {
        ++input_zero_reads;
        return 0; // starvation between partial reads
    }
    ingress_ring *r = sim_ring(input_sim);
    uint8_t msg[PV_MSG_BYTES];
    while (!native_replay && r->ready) {
        ingress_pattern(msg + PV_HEADER_BYTES, input_seq, native_input);
        pv_seal(msg, native_input, input_seq++, PV_PAYLOAD_MAX);
        assert(sim_transfer(input_sim, msg, sizeof msg) == sizeof msg);
    }
    if (n > 997) n = 997; // cut TS packets and SPI messages across calls and frames
    return ingress_read(r, dst, n);
}
void spi_input_stop(void) {
    if (input_sim) {
        // Partial reads must drain immediately. Only an empty read may sleep.
        assert(enc.input_waits == input_zero_reads);
        sim_free(input_sim); input_sim = NULL;
    }
}
void spi_input_print(void) {
    printf(",\"input\":\"%s\"", native_input == PV_TS ? "ts" : native_input ? "gcs" : "synthetic");
}

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
    const size_t offset = (first+bank_shift) % RADIO_S3_BANK_WORDS;
    const size_t parts[] = {RADIO_S3_BANK_WORDS - offset, offset};
    uint64_t at = first;
    for (unsigned p = 0; p < 2; ++p) {
        const size_t count = parts[p];
        if (!count) continue;
        const unsigned sps = dvb.table.n;
        const size_t end_symbol = ((at + count)*dvb.den + sps - 1) / sps;
        const size_t needed = (end_symbol + dvb.ring.frame_symbols - 1) / dvb.ring.frame_symbols;
        // Host correctness test only: wait for the native encoder, with a deadlock timeout.
        unsigned tries = 0;
        while (dvb.lofty ? false : dvb.moon ? dvb.queue_sample+
               (uint32_t)(__atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed)*MACHINE_PAGE_SAMPLES-
               dvb.page_sample < at+count :
               __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE) < needed) {
            assert(tries++ < 5000);
            vTaskDelay(1);
        }
        const unsigned sent = enc.task->notifications_sent;
        if (dvb.moon && getenv("EARLY_WAKE")) xTaskNotifyGive(enc.task);
        assert(fn(banks[b] + (at+bank_shift) % RADIO_S3_BANK_WORDS, at, count, ctx) == ESP_OK);
        if (enc.packed && !dvb.moon) {
            if (offset && !p) assert(enc.task->notifications_sent == sent);
            else assert(!dvb.wake_pending);
        }
        if (dvb.moon) {
            if (!dvb.lofty && !dvb.page_wake && !getenv("EARLY_WAKE")) {
                if (offset && !p) assert(enc.task->notifications_sent == sent);
                else assert(enc.task->notifications_sent-sent <= 1 && !dvb.wake_pending);
            }
            // The independent Python block/convolution oracle checks the whole
            // output, including both cores and every bank/frame seam.
            at += count;
            continue;
        }
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
                assert(banks[b][(at+i+bank_shift)%RADIO_S3_BANK_WORDS] == reference[skip+i]);
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
            assert(banks[b][(at + i+bank_shift) % RADIO_S3_BANK_WORDS] == reference[at % sps + i]);
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
    const uint64_t step = RADIO_S3_STEP_FOR(rate);
    // This raw-payload fixture has no station mux. Timed lofty uses the
    // regular overlap from its first data bank; lofty_handoff.c tests the ID.
    const uint64_t lead = dvb.cached_words-(RADIO_S3_BANK_WORDS-step);
    *stats = (radio_s3_dry_stats_t){0};
    bank_fill(fn, ctx, 0, 0, stats);
    bank_fill(fn, ctx, 1, step - lead, stats);
    for (uint64_t k = 0; k * step < samples; ++k) {
        const uint64_t first = k * step;
        const size_t count = samples - first < step ? (size_t)(samples - first) : (size_t)step;
        if (bank_shift) {
            static uint32_t unshifted[RADIO_S3_BANK_WORDS];
            for (unsigned i=0; i<RADIO_S3_BANK_WORDS; ++i)
                unshifted[i] = banks[k & 1][(i+bank_shift)%RADIO_S3_BANK_WORDS];
            assert(sink_fn(unshifted, first, count, sink_ctx) == ESP_OK);
        } else assert(sink_fn(banks[k & 1], first, count, sink_ctx) == ESP_OK);
        if (first + step < samples)
            bank_fill(fn, ctx, k & 1, (k + 2) * step - lead, stats);
    }
    assert(dvb.ring.underruns == 0);
    return ESP_OK;
}

#ifndef NATIVE_PACE_TEST
esp_err_t radio_s3_paced_run_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn, void *ctx,
                                  radio_s3_stats_t *stats)
{ (void)sec; (void)rate; (void)fn; (void)ctx; (void)stats; abort(); }
esp_err_t radio_s3_paced_wifi_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn, void *ctx, radio_s3_stats_t *stats)
{ (void)sec; (void)rate; (void)fn; (void)ctx; (void)stats; abort(); }
#endif
esp_err_t radio_s3_run_bandwidth(uint32_t freq, uint8_t gain, uint32_t sec, uint32_t rate,
                                 uint32_t bandwidth, radio_s3_fill_fn fn, void *ctx, radio_s3_stats_t *stats)
{ (void)freq; (void)gain; (void)sec; (void)rate; (void)bandwidth; (void)fn; (void)ctx; (void)stats; abort(); }
void radio_filter_print(const radio_filter_stats_t *stats) { (void)stats; }
static unsigned wifi_probes;
static esp_err_t discard(const uint32_t *bank, uint64_t first, size_t count, void *ctx)
{ (void)bank; (void)first; (void)count; (void)ctx; return ESP_OK; }
esp_err_t radio_s3_wifi_probe(radio_s3_stats_t *stats)
{
    assert(enc.fast && enc.ws && enc.payload && (dvb.moon || dvb.win));
    assert(!enc.packed || dvb.samples); // the heap probe must own the playback cache
    memset(stats, 0, sizeof *stats);
    ++wifi_probes;
    return radio_s3_stream_ready();
}

int main(int argc, char **argv)
{
    assert(!posix_memalign((void **)&banks, 65536, 2*sizeof *banks));
    if (getenv("BANK_SHIFT")) bank_shift = strtoul(getenv("BANK_SHIFT"), NULL, 0);
    for (size_t n = 0; n < 18; ++n) {
        uint8_t got[20]; memset(got, 0xa5, sizeof got);
        uint32_t x = SEED, expected = SEED;
        payload_fill(got+1, n, &x);
        for (size_t i = 0; i < n; ++i) assert(got[i+1] == next_byte(&expected));
        assert(x == expected && got[0] == 0xa5 && got[n+1] == 0xa5);
    }
    assert(argc == 7 || argc == 8);
    callsign = argv[4];
    const uint32_t rate = (uint32_t)atol(argv[5]);
    const unsigned mc = (unsigned)atoi(argv[1]), sps = (unsigned)atoi(argv[2]), frames = (unsigned)atoi(argv[6]);
    const bool moon = sps == DVB_SPS_12_5 || (mc >= 12 && sps == 3);
    const bool quads = mc < 12 && rate == 80000000, short_pulse = !moon && mc >= 12 && rate == 80000000;
    if (argc == 8) native_input = atoi(argv[7]);
    if (native_input > 2) { native_replay = true; native_input -= 2; }
    const dvbs2_mode *mode = dvbs2_find_mode(mc);
    const size_t length = dvbs2_frame_symbols(mode, 1), bytes = (mode->kbch - 80) / 8;
    phase_count = (frames + 2) * length;
    phases = malloc(phase_count);
    dvbs2_workspace ws;
    uint8_t payload[8100];
    uint32_t x = SEED;
    dvbs2_ts_state ts = {0};
    uint8_t *source = malloc((frames+4)*bytes + PV_PAYLOAD_MAX);
    if (native_input)
        for (unsigned j = 0; j < (frames+4)*bytes; j += PV_PAYLOAD_MAX)
            ingress_pattern(source+j, native_replay ? (j/PV_PAYLOAD_MAX)%INGRESS_SLOTS : j/PV_PAYLOAD_MAX, native_input);
    for (unsigned f = 0; f < frames + 2; ++f) {
        if (native_input) memcpy(payload, source+f*bytes, bytes);
        else {
            for (size_t i = 0; i < bytes; ++i) payload[i] = next_byte(&x);
            station_payload_header(payload, bytes, callsign);
        }
        assert(!(native_input == PV_TS ? dvbs2_bb_ts(&ws, mode, &ts, payload, bytes) :
                                        dvbs2_bb_gcs_ro(&ws, mode, payload, bytes * 8, sps == DVB_SPS_12_5 ? 2 : 0)));
        dvbs2_bb_scramble(&ws, mode); dvbs2_bch(&ws, mode); dvbs2_ldpc(&ws, mode);
        uint8_t *out = phases + f * length;
        dvbs2_map(&ws, mode, out + 90); dvbs2_plframe(mode, 1, out);
    }
    // Every command-owned allocation can fail: count them on a good start, then fail each.
    // Cleanup must leave no live allocations.
    allocation_count = 0;
    assert(start_encoder("dvbdump", mc, 128, 0, sps, rate));
    const int allocations = allocation_count;
    const bool resident = sps != 40 && (moon || mc < 12 || sps == 4);
    const bool borrowed_bch = mc >= 12 && sps == 4 &&
                              (short_pulse || mc == 13) && mode->t > 8;
    assert(enc.bch_borrowed == borrowed_bch);
    if (borrowed_bch) {
        assert(enc.bch >= enc.filler+packed_bytes(DVBS2_DUMMY_SYMBOLS));
        assert(enc.bch+DVBS2_BCH_CACHE_BYTES <= (uint8_t *)(shaper_store+SHAPER_STORE_WORDS));
        for (unsigned i=0; i<INGRESS_SLOTS && native_buffers[i]; ++i)
            assert(native_buffers[i]+INGRESS_SLOT_BYTES <= enc.bch ||
                   native_buffers[i] >= enc.bch+DVBS2_BCH_CACHE_BYTES);
    }
    if (!moon) assert(allocations == (sps == 40 ? 7 : 4 - (quads && sps == 3) + (mc >= 12 && !short_pulse) +
                          (resident && mode->t > 8 && !native_input && !borrowed_bch) + !resident));
    for (unsigned i = 0; i < 64; ++i) {
        if (native_blocks[i].p == enc.ws)
            assert(native_blocks[i].caps == ((resident ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT));
        if (enc.bch && native_blocks[i].p == enc.bch)
            assert(native_blocks[i].n == DVBS2_BCH_CACHE_BYTES && (native_blocks[i].caps & MALLOC_CAP_INTERNAL));
    }
    if (native_input) {
        // Match the input guide's requested SRAM for every supported mode.
        // DMA/driver/task overhead is outside this host allocator, not measured here.
        size_t internal = 0;
        for (unsigned i = 0; i < 64; ++i)
            if (native_blocks[i].p && (native_blocks[i].caps & MALLOC_CAP_INTERNAL))
                internal += native_blocks[i].n;
        const size_t fast = dvbs2_fast_mode_size(mode);
        const size_t expected = mc >= 12 && sps == 8 ? 22240+2058 :
            sps == 40 ? fast+420 : quads && !moon && sps == 4 ? fast+25056 :
            fast+(quads && sps == 3 ? 0 : 12420)+(mc >= 12 && !short_pulse ? 11120 : 0);
        assert(internal == expected+(sps == 40 ? 0 : CACHED_FOR(rate)*sizeof(uint32_t)));
        assert((native_buffers[0] != NULL) == !((quads && sps == 3) || (mc >= 12 && (sps == 8 || (mc == 13 && !short_pulse)))));
    }
    if (enc.packed && !resident) {
        assert(enc.filler == (uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED);
        assert(enc.filler+packed_bytes(DVBS2_DUMMY_SYMBOLS) <= (uint8_t *)(enc.ws+1));
    }
    if (quads && !moon && sps == 4) assert((uint32_t *)enc.ws == shaper_store+QPSK_TABLE_WORDS(4));
    assert(!((uintptr_t)enc.fast & 15) && !((uintptr_t)enc.ws & 15));
    if (resident && !moon) assert(dvb.win == (uint8_t *)enc.ws + DVBS2_FAST_WORKSPACE_USED);
    assert(dvb.win_bytes <= (moon ? 6864u : sps == 3 ? 1370u : sps == 4 ? 4105u : sps == 8 ? 2058u : 420u));
    stop_encoder();
    printf("}\n");
    assert(live_allocations == 0);
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder("dvbdump", mc, 128, 0, sps, rate));
        assert(live_allocations == 0);
    }
    fail_after = -1;
    if (dvb.lofty) {
        native_task_fail = 1;
        assert(!start_encoder("dvbdump",mc,128,0,sps,rate));
        assert(!live_allocations);
        native_task_fail = 0;
    }
    if (*callsign) {
        dvbtx_wifi(mc, sps, rate);
        assert(wifi_probes == 1 && live_allocations == 0);
        assert((dvb.table.parts == 2) == short_pulse);
    }
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        capture = fopen(argv[3], "wb"); assert(capture);
        dvbtx_dump(mc, frames, 128, sps == 40 ? 10 : mc < 12 && sps == 8 ? 2 : 1, 0, sps, rate);
        fclose(capture);
        assert(live_allocations == 0);
        for (unsigned i = 0; i < 4; ++i) assert(native_ranges[i] == 0);
    }
    if (!native_input && (mc == 11 || mc == 13 || mc == 15 || mc == 17)) {
        // Legacy pace uses main's contiguous stream. New modes copy one repeated
        // internal frame. Compare actual output through many source/frame wraps.
        x = SEED;
        for (unsigned f = 0; f < frames+2; ++f) {
            if (moon) x = SEED;
            for (size_t i = 0; i < bytes; ++i) payload[i] = next_byte(&x);
            station_payload_header(payload,bytes,callsign);
            assert(!dvbs2_bb_gcs_ro(&ws,mode,payload,bytes*8,sps == DVB_SPS_12_5 ? 2 : 0));
            dvbs2_bb_scramble(&ws,mode); dvbs2_bch(&ws,mode); dvbs2_ldpc(&ws,mode);
            dvbs2_map(&ws,mode,phases+f*length+90); dvbs2_plframe(mode,1,phases+f*length);
        }
        allocation_count = 0;
        assert(start_encoder("dvbpace",mc,128,0,sps,rate));
        const int pace_allocations = allocation_count;
        const bool heap_input = enc.input && !enc.input_borrowed;
        assert(pace_allocations == allocations+(int)heap_input);
        assert((enc.input != NULL) == moon);
        assert(enc.warmup_frames == (moon ? 3u : 0u));
        if (moon) {
            if (quads) {
                assert(!enc.input_borrowed);
            } else {
                assert(!enc.input_borrowed);
                for (unsigned i = 0; i < 64; ++i)
                    if (native_blocks[i].p == enc.input)
                        assert(native_blocks[i].caps & (dvb.lofty ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL));
            }
            x = SEED;
            for (size_t i = 0; i < bytes; ++i) {
                size_t prefix = dvb.lofty ? LOFTY_INPUT_BYTES : 0;
                assert((i < prefix ? lofty_input[i] : enc.input[i-prefix]) == next_byte(&x));
            }
        }
        radio_s3_dry_stats_t stats;
        assert(!radio_s3_dry_run_rate(fill,NULL,((uint64_t)frames*length*sps+dvb.den-1)/dvb.den,
                                      rate,discard,NULL,&stats));
        stop_encoder(); printf("}\n");
        assert(live_allocations == 0);
        if (heap_input) {
            fail_after = pace_allocations-1;
            assert(!start_encoder("dvbpace",mc,128,0,sps,rate));
            assert(live_allocations == 0);
            fail_after = -1;
        }
    }
    if (native_input) {
        input_empty = true;
        assert(start_encoder("dvbdump", mc, 128, 0, sps, rate));
        assert(enc.frames == 0 && !enc.failed);
        uint8_t window[64];
        assert(ring_window(&dvb.ring, window, sizeof window, 0, 12, sps, 0, RADIO_S3_BANK_WORDS) >= 0);
        assert(dvb.ring.underruns > 0);
        input_empty = false;
        unsigned tries=0;
        while (!enc.frames) { assert(++tries < 5000); vTaskDelay(1); }
        assert(!enc.failed && enc.input_partial > 0);
        stop_encoder(); printf("}\n");
        assert(live_allocations == 0);
    }
    free(source); free(phases); free(banks);
    return 0;
}
