// Actual dvbtx.c lifecycle with pthread task/notification stubs, no peripherals.
// Timing is synthetic. Compare every filled DAC word against the plain encoder.
#include "native.h"
#include <stdbool.h>
#include "run_memory.h"
/* Host run memory: native.h's heap model (counts, injected failures, caps) behind the firmware's
 * arena capacity, which the firmware's allocator core and first-fit policy check here. */
static arena_t native_run[3] = {{(uint8_t *)RUN_LOW_BASE, RUN_LOW_BYTES, 0, 0},
                                {(uint8_t *)RUN_HIGH_BASE, RUN_HIGH_BYTES, 0, 0},
                                {(uint8_t *)0x3d800000, RUN_PSRAM_BYTES, 0, 0}};
static void *native_taken[64];
static unsigned native_takes;
static pthread_mutex_t native_run_lock = PTHREAD_MUTEX_INITIALIZER;
void *run_take(size_t bytes, size_t align, uint32_t caps) {
    if (align < 16) align = 16;
    pthread_mutex_lock(&native_run_lock);
    void *p = NULL;
    const int fits = caps & MALLOC_CAP_SPIRAM ? arena_take_aligned(&native_run[2], bytes, align) != NULL :
        arena_take_aligned(&native_run[0], bytes, align) || arena_take_aligned(&native_run[1], bytes, align);
    if (fits && (p = heap_caps_aligned_alloc(align, bytes, caps))) {
        memset(p, 0, bytes);
        assert(native_takes < 64); native_taken[native_takes++] = p;
    }
    pthread_mutex_unlock(&native_run_lock);
    return p;
}
void run_reset(void) {
    pthread_mutex_lock(&native_run_lock);
    while (native_takes) heap_caps_free(native_taken[--native_takes]);
    for (unsigned i = 0; i < 3; ++i) native_run[i].used = 0;
    pthread_mutex_unlock(&native_run_lock);
}
// A command's run memory is returned by the next command's run_reset. Return it now and check
// that nothing else is left on the (modelled) heap.
static bool run_returned(void) { run_reset(); return !live_allocations; }
void run_memory_print(void) {
    printf(",\"run_memory\":{\"low\":[%u,%u,%u],\"high\":[%u,%u,%u],\"psram\":[%u,%u,%u]}",
           (unsigned)native_run[0].used, (unsigned)native_run[0].peak, (unsigned)native_run[0].size,
           (unsigned)native_run[1].used, (unsigned)native_run[1].peak, (unsigned)native_run[1].size,
           (unsigned)native_run[2].used, (unsigned)native_run[2].peak, (unsigned)native_run[2].size);
}
#include "ring.h"
#include "machine.h"
uint32_t native_ranges[4];
static int checked_window(ring_t *, uint8_t *, size_t, uint64_t, size_t, unsigned, unsigned, unsigned);
static int checked_task(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
static TaskHandle_t checked_static_task(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                                        unsigned priority, StackType_t *buffer, StaticTask_t *tcb, int core)
{
    (void)buffer; (void)tcb; TaskHandle_t handle = NULL;
    return checked_task(fn, name, stack, arg, priority, &handle, core) == pdPASS ? handle : NULL;
}
static uint32_t checked_load(const volatile uint32_t *, int);
#define __atomic_load_n checked_load
#define xTaskCreatePinnedToCore checked_task
#define xTaskCreateStaticPinnedToCore checked_static_task
#define ring_window checked_window
#include "dvbtx.c"
#undef ring_window
#undef xTaskCreateStaticPinnedToCore
#undef xTaskCreatePinnedToCore
#undef __atomic_load_n

static uint32_t checked_load(const volatile uint32_t *p, int order)
{
    static unsigned forced_mask;
    static unsigned forced_frame;
    static bool forced_admission;
    if (p == &dvb.produced && !current && forced_admission) {
        forced_admission = false;
        return dvb.consumed;
    }
    if (p == &lofty_mux.published && !current && !getenv("UNPACED_LOFTY")) {
        // Host-only producer pacing at admission, without changing physical
        // bank callbacks. The hardware consumer never waits here.
        if (!lofty_mux.output) { forced_mask = 0; forced_frame = UINT32_MAX; }
        const char *frame = getenv("FORCE_DUMMY_FRAME");
        const char *from = getenv("DUMMY_FROM_FRAME");
        const char *period_text = getenv("DUMMY_FRAME_PERIOD");
        const unsigned period = period_text ? strtoul(period_text,NULL,0) : 1;
        assert(period);
        const bool each = from && lofty_mux.frames >= strtoul(from,NULL,0) &&
                          (lofty_mux.frames-strtoul(from,NULL,0))%period == 0 &&
                          lofty_mux.frames != forced_frame;
        unsigned bit = frame ? lofty_mux.frames == strtoul(frame,NULL,0) :
                       lofty_mux.frames == 0 ? 1 : lofty_mux.frames == 1 ? 2 : lofty_mux.frames == 5 ? 4 : 0;
        if (each || ((frame || getenv("FORCE_DUMMIES")) && bit && !(forced_mask&bit))) {
            forced_mask |= bit;
            forced_frame = lofty_mux.frames;
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
        // Forced not-ready admissions replay the dummy path inside a dump.
        if (getenv("FORCE_DUMMIES") || getenv("FORCE_DUMMY_FRAME") || getenv("DUMMY_FROM_FRAME"))
            lofty_mux.whole_frames = false;
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
static FILE *native_wire;
static unsigned native_input, input_seq, input_reads, input_zero_reads;
static volatile bool input_empty;
static size_t input_limit = SIZE_MAX, input_given; // bytes before the source stops
static size_t input_corrupt = SIZE_MAX;             // zero the input from here: BB rejects the TS
static bool fill_may_fail;                          // whole-frame tests: return fill errors
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
static bool (*native_control)(unsigned);
void spi_input_control(bool (*control)(unsigned)) { native_control=control; }
unsigned spi_input_type(void) { return native_input; }
const char *spi_input_source(void) { return "spi"; }
esp_err_t spi_input_start(void) {
    if (native_input) {
        // Charge run memory as spi_input_start does on the board: the slot block unless the
        // encoder lent slots. The 4 KiB CRC table came from the heap with spiinput.
        uint8_t *slots[INGRESS_SLOTS];
        memcpy(slots, native_buffers, sizeof slots);
        if (!slots[0]) {
            uint8_t *block = run_take(INGRESS_SLOTS*INGRESS_SLOT_BYTES, 16, MALLOC_CAP_INTERNAL);
            if (!block) return ESP_ERR_NO_MEM;
            for (unsigned i = 0; i < INGRESS_SLOTS; ++i) slots[i] = block+i*INGRESS_SLOT_BYTES;
        }
        input_sim = sim_new(); sim_ring(input_sim)->type = native_input;
        sim_ring(input_sim)->control=native_control;
        for (unsigned i = 0; i < INGRESS_SLOTS; ++i) sim_set_buffer(input_sim, i, slots[i]);
        if (native_replay) sim_replay(input_sim, native_input);
        input_seq = input_reads = input_zero_reads = 0;
    }
    return ESP_OK;
}
size_t spi_input_read(uint8_t *dst, size_t n) {
    if (input_empty || input_given >= input_limit || (++input_reads % 7) == 0) {
        ++input_zero_reads;
        return 0; // starvation between partial reads
    }
    ingress_ring *r = sim_ring(input_sim);
    uint8_t msg[PV_MSG_BYTES];
    while (!native_replay && r->ready) {
        if (native_wire) {
            const size_t count=fread(msg,1,sizeof msg,native_wire);
            if (!count) break;
            assert(count == sizeof msg);
        } else {
            ingress_pattern(msg + PV_HEADER_BYTES, input_seq, native_input);
            pv_seal(msg, native_input, input_seq++, PV_PAYLOAD_MAX);
        }
        assert(sim_transfer(input_sim, msg, sizeof msg) == sizeof msg);
    }
    if (n > 997) n = 997; // cut TS packets and SPI messages across calls and frames
    if (n > input_limit-input_given) n = input_limit-input_given;
    const size_t got = ingress_read(r, dst, n);
    for (size_t i = 0; i < got; ++i)
        if (input_given+i >= input_corrupt) dst[i] = 0;
    input_given += got;
    return got;
}
void spi_input_stop(void) {
    if (input_sim) {
        // Partial reads must drain immediately. Only an empty read may sleep.
        assert(fill_may_fail || enc.input_waits == input_zero_reads);
        sim_free(input_sim); input_sim = NULL;
    }
}
void spi_input_print(void) {
    printf(",\"input\":\"%s\"", native_input == PV_TS ? "ts" : native_input ? "gcs" : "synthetic");
}
const ingress_ring *spi_input_ring(void) { return input_sim ? sim_ring(input_sim) : NULL; }

// Station store and command gate: the stored callsign without NVS.
const char *station_store_get(void) { return callsign; }
bool station_require(const char *cmd) { (void)cmd; return callsign[0] != 0; }

int usb_serial_jtag_write_bytes(const void *buf, size_t n, unsigned timeout)
{
    (void)timeout;
    assert(fwrite(buf, 1, n, capture) == n);
    return (int)n;
}

static bool vcm_ready(size_t end)
{
    const ring_t *r=&dvb.ring;
    const unsigned published=__atomic_load_n(&r->published,__ATOMIC_ACQUIRE);
    uint64_t available=r->chosen_end;
    for (unsigned f=r->next_frame;f!=published;++f) available+=vcm_enc.lengths[f%r->nslots];
    return available >= end;
}

static esp_err_t bank_fill(radio_s3_fill_fn fn, void *ctx, unsigned b, uint64_t first,
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
        while (vcm_enc.vcm ? !vcm_ready(end_symbol) : dvb.lofty ? false : dvb.moon ? dvb.queue_sample+
               (uint32_t)(__atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed)*MACHINE_PAGE_SAMPLES-
               dvb.page_sample < at+count :
               __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE) < needed) {
            assert(tries++ < 5000);
            vTaskDelay(1);
        }
        if (dvb.moon && getenv("EARLY_WAKE")) xTaskNotifyGive(enc.task);
        const unsigned sent = enc.task->notifications_sent;
        const esp_err_t filled = fn(banks[b] + (at+bank_shift) % RADIO_S3_BANK_WORDS, at, count, ctx);
        if (fill_may_fail && filled != ESP_OK) return filled;
        assert(filled == ESP_OK);
        if (enc.packed && !dvb.moon) {
            if (offset && !p) assert(enc.task->notifications_sent == sent);
            else assert(!dvb.wake_pending);
        }
        if (dvb.moon) {
            if (!dvb.lofty && !getenv("EARLY_WAKE")) {
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
        shaper_table_t oracle = dvb.table;
        if (dvb.table.parts == SHAPER_AXES_PARTS) {
            static uint32_t full[SHAPER_STORE_WORDS];
            static bool built;
            if (!built) {
                int32_t taps[72];
                for (unsigned i=0;i<72;++i)
                    taps[i]=(int32_t)((double)shaper_taps_8psk_n8_a35[i]*128/WORST_PEAK);
                shaper_build(&oracle,full,taps,8,3);
                built=true;
            } else oracle=(shaper_table_t){.blocks=full,.n=8,.bits=3,.block_words=32};
        }
        shaper_fill_c(reference, shifted + SHAPER_HISTORY,
                       (int)(symbols - SHAPER_HISTORY), &oracle);
        for (size_t i = 0; i < count; ++i)
            assert(banks[b][(at + i+bank_shift) % RADIO_S3_BANK_WORDS] == reference[at % sps + i]);
        at += count;
    }
    if (dvb.ring.packed) {
        assert(dvb.shaped_words == dvb.sample_end);
        assert(dvb.sample_end == first + RADIO_S3_BANK_WORDS);
    }
    stats->fills++;
    return ESP_OK;
}

// The bank sequence of radio_s3.c's dry_worker at the rate's geometry (radio_s3.h).
esp_err_t radio_s3_dry_run_rate(radio_s3_fill_fn fn, void *ctx, uint64_t samples,
                                uint32_t rate, radio_s3_sink_fn sink_fn, void *sink_ctx,
                                radio_s3_dry_stats_t *stats)
{
    const uint64_t step = RADIO_S3_STEP_FOR(rate);
    // This raw-payload fixture has no station mux. Timed lofty uses the
    // regular overlap from its first data bank; psk8_handoff.c tests the ID.
    const uint64_t lead = dvb.cached_words-(RADIO_S3_BANK_WORDS-step);
    *stats = (radio_s3_dry_stats_t){0};
    esp_err_t error = bank_fill(fn, ctx, 0, 0, stats);
    if (!error) error = bank_fill(fn, ctx, 1, step - lead, stats);
    if (error) return error;
    for (uint64_t k = 0; k * step < samples; ++k) {
        const uint64_t first = k * step;
        const size_t count = samples - first < step ? (size_t)(samples - first) : (size_t)step;
        if (bank_shift) {
            static uint32_t unshifted[RADIO_S3_BANK_WORDS];
            for (unsigned i=0; i<RADIO_S3_BANK_WORDS; ++i)
                unshifted[i] = banks[k & 1][(i+bank_shift)%RADIO_S3_BANK_WORDS];
            assert(sink_fn(unshifted, first, count, sink_ctx) == ESP_OK);
        } else assert(sink_fn(banks[k & 1], first, count, sink_ctx) == ESP_OK);
        if (first + step < samples && (error = bank_fill(fn, ctx, k & 1, (k + 2) * step - lead, stats)))
            return error;
    }
    assert(dvb.ring.underruns == 0);
    return ESP_OK;
}

#ifndef NATIVE_PACE_TEST
esp_err_t radio_s3_paced_run(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn, void *ctx,
                             const radio_s3_options_t *options, radio_s3_stats_t *stats)
{ (void)sec; (void)rate; (void)fn; (void)ctx; (void)options; (void)stats; abort(); }
#endif
static bool native_keyed_fixture;
static unsigned native_keyed_frames;
static esp_err_t discard(const uint32_t *, uint64_t, size_t, void *);
esp_err_t radio_s3_run_options(uint32_t freq, uint8_t gain, uint32_t sec, uint32_t rate,
                               uint32_t bandwidth, radio_s3_fill_fn fn, void *ctx,
                               const radio_s3_options_t *options, radio_s3_stats_t *stats)
{
    // Public keyed API, with the radio replaced by the actual dry fill/oracle.
    assert(native_keyed_fixture && freq == 2412 && !gain && sec == 30 && rate == 80000000);
    assert(bandwidth == (dvb.lofty ? 36000000u : 40000000u));
    assert(enc.input && !spi_input_type() && options && options->status && !options->startup);
    uint32_t x = SEED;
    for (size_t i = 0; i < (enc.mode->kbch-80U)/8; ++i) assert(enc.input[i] == next_byte(&x));
    if (dvb.lofty) assert(radio_s3_stream_ready() == ESP_OK);
    const uint64_t samples = ((uint64_t)native_keyed_frames*dvb.ring.frame_symbols*dvb.table.n+dvb.den-1)/dvb.den;
    radio_s3_dry_stats_t dry;
    memset(stats,0,sizeof *stats);
    const esp_err_t err = radio_s3_dry_run_rate(fn,ctx,samples,rate,discard,NULL,&dry);
    stats->fills = dry.fills;
    // One status line after the frames played, as the waiting command task would print it,
    // to stdout (the dump capture is closed by now). The test reads it with the run record.
    stats->late_fills = 1;
    FILE *dump = capture;
    capture = stdout;
    options->status(stats,23610);
    capture = dump;
    stats->late_fills = 0;
    return err;
}
void radio_filter_print(const radio_filter_stats_t *stats) { (void)stats; }
void radio_s3_tuning_print(const radio_s3_stats_t *stats) { (void)stats; }
static esp_err_t discard(const uint32_t *bank, uint64_t first, size_t count, void *ctx)
{ (void)bank; (void)first; (void)count; (void)ctx; return ESP_OK; }

static void id_handoff_check(uint32_t rate)
{
    station_id_t id;
    assert(station_id_init_rate(&id,callsign,rate,rate,fill,NULL));
    const uint64_t step = RADIO_S3_STEP_FOR(rate), lead = RADIO_S3_LEAD_FOR(rate);
    const uint64_t first = (id.length+lead)/step*step-lead;
    assert(first < id.length && first+RADIO_S3_BANK_WORDS > id.length);
    const unsigned sent = enc.task->notifications_sent;
    const size_t offset = first%RADIO_S3_BANK_WORDS;
    const size_t head = RADIO_S3_BANK_WORDS-offset;
    assert(station_id_fill(banks[0]+offset,first,head,&id) == ESP_OK);
    if (offset) assert(station_id_fill(banks[0],first+head,offset,&id) == ESP_OK);
    assert(enc.task->notifications_sent-sent <= 1);
    const size_t samples = first+RADIO_S3_BANK_WORDS-id.length;
    assert(dvb.queue_sample == samples && dvb.queue_empty == 0);
    // Independent packed-phase path checks payload at the exact CW/data seam.
    uint8_t packed[1800] = {0};
    const unsigned groups = (samples+11)/12;
    for (unsigned j = 6; j < 6+5*groups+8; ++j)
        packed[j/4] |= (phases[j-6] >> 1) << (6-2*(j&3));
    machine_reference(reference,packed,0,groups,shaper_store);
    for (unsigned j = 0; j < samples; ++j)
        assert(banks[0][(id.length+j)%RADIO_S3_BANK_WORDS] == reference[j]);
}


// Whole-frame dump admission (lofty_consumer.inc): with no sender, a source that stops in the
// middle of a frame, or input the encoder rejects, a dump ends with ESP_FAIL instead of waiting
// forever. The harness must not pace publication here (UNPACED_LOFTY).
static void whole_frame_failures(unsigned mc, unsigned sps, uint32_t rate, size_t frame_bytes,
                                 size_t symbols)
{
    setenv("UNPACED_LOFTY", "1", 1);
    fill_may_fail = true;
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        input_empty = scenario == 0;
        input_given = 0;
        input_limit = scenario == 1 ? 2*frame_bytes+frame_bytes/2 : SIZE_MAX;
        input_corrupt = scenario == 2 ? 3*frame_bytes+frame_bytes/2 : SIZE_MAX;
        capture = tmpfile(); assert(capture);
        const TickType_t started = xTaskGetTickCount();
        dvbtx_dump(mc, 16, 128, 1, 0, sps, rate);
        const TickType_t waited = xTaskGetTickCount()-started;
        // The host still receives the announced binary_bytes, zero-padded (board.py framing).
        assert((uint64_t)ftell(capture) == (16*(uint64_t)symbols*dvb.table.n+dvb.den-1)/dvb.den*4);
        fclose(capture);
        assert(run_returned());
        assert(lofty_mux.whole_frames);
        // Every scenario ends; only the timeout waits out the bound, and not much longer.
        assert(scenario == 2 ? !lofty_mux.whole_frame_timeout && waited < LOFTY_WHOLE_WAIT_MS :
               lofty_mux.whole_frame_timeout && waited >= LOFTY_WHOLE_WAIT_MS);
        assert(waited < LOFTY_WHOLE_WAIT_MS+2000);
        printf("@{\"test\":\"whole_frame_failure\",\"scenario\":\"%s\",\"ms\":%u,"
               "\"published\":%u,\"timeout\":%s}\n",
               (const char *[]){"no input","input stops mid-frame","encoder fails"}[scenario],
               (unsigned)waited, (unsigned)lofty_mux.published, lofty_mux.whole_frame_timeout ? "true" : "false");
        static const unsigned published[] = {0, 2, 3};
        assert(lofty_mux.published == published[scenario]);
    }
    input_empty = fill_may_fail = false;
    input_limit = input_corrupt = SIZE_MAX;
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
    // MODCOD 100 + n selects the short-FECFRAME mode n (1 Msym/s QPSK only).
    const bool short_frames = atoi(argv[1]) >= 100;
    dvbtx_set_short(short_frames);
    const unsigned mc = (unsigned)atoi(argv[1]) % 100, sps = (unsigned)atoi(argv[2]), frames = (unsigned)atoi(argv[6]);
    const bool moon = sps == DVB_SPS_12_5 || (mc >= 12 && sps == 3);
    const bool quads = mc < 12 && rate == 80000000, short_pulse = !moon && mc >= 12 && rate == 80000000;
    if (argc == 8) native_input = atoi(argv[7]);
    if (native_input > 2) { native_replay = true; native_input -= 2; }
    const dvbs2_mode *mode = short_frames ? dvbs2_find_mode_short(mc) : dvbs2_find_mode(mc);
    const size_t length = dvbs2_frame_symbols(mode, 1), bytes = (mode->kbch - 80) / 8;
    if (getenv("WHOLE_FRAME")) {
        assert(native_input && moon && mc >= 12);
        whole_frame_failures(mc, sps, rate, bytes, length);
        return 0;
    }
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
        assert(!(native_input == PV_TS ? dvbs2_bb_ts_ro(&ws, mode, &ts, payload, bytes, sps == DVB_SPS_12_5 ? 2 : 0) :
                                        dvbs2_bb_gcs_ro(&ws, mode, payload, bytes * 8, sps == DVB_SPS_12_5 ? 2 : 0)));
        dvbs2_bb_scramble(&ws, mode); dvbs2_bch(&ws, mode); dvbs2_ldpc(&ws, mode);
        uint8_t *out = phases + f * length;
        dvbs2_map(&ws, mode, out + 90); dvbs2_plframe(mode, 1, out);
    }
    // Every command-owned allocation can fail: count them on a good start, then fail each.
    // Cleanup must leave no live allocations.
    allocation_count = 0;
    if (!moon || native_input) {
        assert(!start_encoder_options("dvb",mc,128,0,sps,rate,true,true));
        assert(!allocation_count && run_returned());
    }
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
    const int input_takes = native_input && !native_buffers[0]; // the slot block
    if (!moon) assert(allocations == input_takes + (sps == 40 ? 7 : 4 + (mc >= 12 && !short_pulse) +
                          (resident && (mode->t > 8 || mc >= 12) && (!native_input || quads) && !borrowed_bch) + !resident));
    for (unsigned i = 0; i < 64; ++i) {
        if (native_blocks[i].p == enc.ws)
            assert(!(native_blocks[i].caps & MALLOC_CAP_SPIRAM) == resident);
        if (enc.bch && native_blocks[i].p == enc.bch)
            assert(native_blocks[i].n == DVBS2_BCH_CACHE_BYTES && (native_blocks[i].caps & MALLOC_CAP_INTERNAL));
    }
    if (native_input) {
        // Match the input guide's requested SRAM for every supported mode, including the
        // input's own slot block (unless borrowed). The CRC table, driver and task overhead
        // are outside this host allocator.
        size_t internal = 0;
        for (unsigned i = 0; i < 64; ++i)
            if (native_blocks[i].p && (native_blocks[i].caps & MALLOC_CAP_INTERNAL))
                internal += native_blocks[i].n;
        const size_t fast = dvbs2_fast_mode_size(mode);
        // 33.33 Msym/s (moon): the context and its resident encoder cache; the workspace
        // lives in the renderer store.
        const size_t expected = moon ? fast+dvbs2_fast_cache_size(enc.fast) :
            mc >= 12 && sps == 8 ? 22240+2058 :
            sps == 40 ? fast+420 : quads && !moon && sps == 4 ? fast+25056 :
            fast+(quads && sps == 3 ? 8352 : 12420)+(mc >= 12 && !short_pulse ? 11120 : 0);
        // QPSK at 80 MS/s keeps its 8 KiB byte BCH table with input (t = 10/12).
        const size_t byte_bch = quads && !moon && mode->t > 8 ? DVBS2_BCH_CACHE_BYTES : 0;
        // Eight slots fit the slow QPSK tails and short-pulse 8PSK 8/9 or
        // 9/10 tails. All other layouts take the slot block from run memory.
        const bool borrowed = mc < 12 ? sps == 8 || sps == 40 : short_pulse && mc >= 16;
        const size_t ingress = borrowed ? 0 : INGRESS_SLOTS*INGRESS_SLOT_BYTES;
        assert(internal == expected+byte_bch+ingress+(sps == 40 ? 0 : CACHED_FOR(rate)*sizeof(uint32_t)));
        assert((native_buffers[0] != NULL) == borrowed);
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
    assert(run_returned());
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder("dvbdump", mc, 128, 0, sps, rate));
        assert(run_returned());
    }
    fail_after = -1;
    if (dvb.lofty) {
        native_task_fail = 1;
        assert(!start_encoder("dvbdump",mc,128,0,sps,rate));
        assert(run_returned());
        native_task_fail = 0;
    }
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        capture = fopen(argv[3], "wb"); assert(capture);
        dvbtx_dump(mc, frames, 128, sps == 40 ? 10 : mc < 12 && sps == 8 ? 2 : 1, 0, sps, rate);
        fclose(capture);
        assert(run_returned());
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
        // Queued paces here use the prepared-frame fixture (dvbpace --fixture).
        assert(start_encoder_options("dvbpace",mc,128,0,sps,rate,false,moon));
        const int pace_allocations = allocation_count;
        const bool heap_input = enc.input && !enc.input_borrowed;
        assert(pace_allocations == allocations+(int)heap_input);
        assert((enc.input != NULL) == moon);
        assert(enc.warmup_frames == (moon ? 3u : 0u));
        if (moon) {
            if (dvb.lofty) {
                assert(!enc.input_borrowed && esp_ptr_external_ram(enc.input));
            } else {
                assert(enc.input_borrowed);
                assert(enc.prefill_frames >= 2);
                assert(dvb.axis_history == 0);
                assert(dvb.render_scratch+MACHINE_AXIS_CHUNK_SAMPLES <= (uint32_t *)enc.ws);
                assert(enc.input >= (uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED);
                assert(enc.input+bytes <= (uint8_t *)(shaper_store+SHAPER_STORE_WORDS));
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
        assert(run_returned());
        if (moon) {
            native_keyed_fixture = true;
            native_keyed_frames = frames;
            dvbtx_run(2412,mc,30,0,128,sps,rate,true);
            assert(run_returned() && !enc.failed);
            native_keyed_fixture = false;
        }
        if (moon && !dvb.lofty && mc == 11) {
            assert(start_encoder_options("dvbpace",mc,128,0,sps,rate,false,true));
            id_handoff_check(rate);
            stop_encoder(); printf("}\n");
            assert(run_returned());
        }
        if (heap_input) {
            fail_after = pace_allocations-1;
            assert(!start_encoder_options("dvbpace",mc,128,0,sps,rate,false,moon));
            assert(run_returned());
            fail_after = -1;
        }
    }
    if (native_input) {
        input_empty = true;
        assert(start_encoder("dvbdump", mc, 128, 0, sps, rate));
        assert(enc.frames == 0 && !enc.failed);
        if (!moon) { // the page queue (moon) has no ring to underrun
            uint8_t window[64];
            assert(ring_window(&dvb.ring, window, sizeof window, 0, 12, sps, 0, RADIO_S3_BANK_WORDS) >= 0);
            assert(dvb.ring.underruns > 0);
        }
        input_empty = false;
        unsigned tries=0;
        while (!enc.frames) { assert(++tries < 5000); vTaskDelay(1); }
        assert(!enc.failed && enc.input_partial > 0);
        stop_encoder(); printf("}\n");
        assert(run_returned());
    }
    free(source); free(phases); free(banks);
    return 0;
}
