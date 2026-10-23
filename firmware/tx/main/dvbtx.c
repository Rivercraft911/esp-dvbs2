// QPSK defaults to 1 Msym/s; both modulations accept 5m/10m at 40 MS/s and
// 20m at 80 MS/s. Fast rates use packed internal rings and cached samples.
// Paired QPSK n4/n8 tables share the store with ring and payload. The block
// kernels use four codes per byte; n3 shares with its ring, n4 with its workspace.
// Payload: xorshift32 seed 2026, one contiguous byte stream across data frames.
// On underrun the ring inserts a 3330-symbol dummy PLFRAME (EN 302 307-1 5.1.5,
// 5.5.1), preserving each choice for overlapping bank reads. No payload is skipped.
// Legacy RRC roll-off 0.35 and fractional 0.20 match the BBHEADER. Legacy scaling uses the
// conservative bound sqrt(2)*508 LSB for every table (archived host/shaper.py Profile.taps).
// The taps divide out the DAC's sample hold (archived host/shaper.py corrected_rrc) because the
// radio sends the held signal; a raw dvbdump has no hold and measures differently.

#include "dvbtx.h"
#include "spi_input.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "dvbs2.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radio_s3.h"
#include "ring.h"
#include "vcm_sections.h"
#include "station_id.h"
#include "station_store.h"
#include "station_commands.h"
#include "shaper.h"
#include "shaper_qpsk.h"
#include "shaper_taps.h"
#include "machine.h"
#include "lofty.h"

#define RING_SLOTS 8                // legacy QPSK frames ahead: 233 ms at 1 Msym/s
#define PACKED_SLOTS 3              // fast rates: three packed frames at 4 sps, two at 8, plus a cache
#define PILOTS 1

// Short FECFRAMEs (16,200 bits) for the next dvb, dvbdump or dvbpace run: QPSK at 1 or 5 Msym/s, 8PSK at 5 Msym/s.
static bool short_frames_enabled;
void dvbtx_set_short(bool enabled) { short_frames_enabled = enabled; }
static const dvbs2_mode *tx_find_mode(unsigned modcod)
{
    return short_frames_enabled ? dvbs2_find_mode_short(modcod) : dvbs2_find_mode(modcod);
}
#define BANK_SYMBOLS 4096           // ceil(16380/4) + 1, worst offset at 4 sps
#define TAPS (9 * 40)
#define WORST_PEAK 718.4            // conservative sqrt(2)*508 LSB axis bound, before rounding
#define SEED 2026
#define ENCODER_PRIORITY 5          // above the command loop, below the IPC tasks
#define MOON_PRIORITY (configMAX_PRIORITIES - 1) // bounded encoder bursts, then queue backpressure
#define ENCODER_STACK 2048
// Samples, including the first bank's extra lead. Later banks re-read 2112 words, or 640 on the
// 80 MS/s geometry, which keeps only its 768 (dvb.cached_words).
#define CACHED_FOR(rate) (RADIO_S3_BANK_WORDS - RADIO_S3_STEP_FOR(rate) + RADIO_S3_LEAD_FOR(rate))
// Finished samples cover the overlap. The next fresh sample only needs filter history and
// one symbol cut at each end, even when the output wraps in the middle of an 8-sps symbol.
#define OVERLAP_SYMBOLS 16 // twelve history symbols plus cut edges in the rational path

typedef struct {
    ring_t ring;
    shaper_table_t table;
    uint8_t *win;                  // internal; borrows the tail of each internal fast workspace
    size_t win_bytes;
    uint8_t overlap[OVERLAP_SYMBOLS];
    uint32_t *samples;             // active-rate overlap; heap probe retains the complete layout
    uint64_t sample_end;            // cached samples end here; only advances on fresh samples
    size_t sample_at;               // circular cache index of sample_end
    uint32_t max_fill_cycles;       // one fill call
    bool profile;                  // radio-off commands only
    unsigned den, history, span;   // exact sps = table.n / den
    bool moon;                    // core-0 sign mapper / core-1 renderer
    bool lofty;
    unsigned queue_pages, wake_pages;
    lofty_page *lofty_pages;
    uint16_t last_word;
    machine_page *pages;           // borrowed shaper-store tail
    uint32_t produced, consumed;   // single writer each; release/acquire ownership
    unsigned put_index, get_index, page_blocks, page_words;
    dvbs2_sign_state mapper;
    uint32_t *render_scratch;       // 192 sample words for physical bank cuts
    uint32_t axis_history;          // previous page's final sixteen signs per axis
    uint64_t queue_wait_cycles;
    uint32_t queue_wait_max, queue_wait_live_max, prepare_ticks, page_clock, page_gap_max;
    uint64_t page_wait;
    uint64_t queue_sample;
    unsigned page_sample;          // consumer cursor in its current page
    unsigned waiting;              // producer is asleep; consumer wakes once below threshold
    uint32_t queue_low, queue_empty, queue_wakes;
    uint32_t producer_phase, wait_put, wait_get;
    struct { uint32_t put, get, phase, wait_put, wait_get; } queue_fault;
    uint32_t wake_bank, wake_max;
    bool wake_pending;             // released storage; notify only after the whole bank
    uint32_t prepare_max;
    uint32_t prepare_tail_max;
    uint64_t prepare_cycles, read_cycles; // page loop: all work, and its framed-word reads
    uint32_t prepare_pages;
    uint32_t dac_rate;             // the run's bank geometry
    size_t cached_words;           // active sample cache length
    stream_profile_t copy, whole, partial, save, notify;
    uint64_t fill_words, fill_cycles;
    uint32_t fill_calls;
    uint32_t bank_cycles, bank_whole, bank_calls;
    uint32_t max_bank_cycles, max_bank_whole, max_bank_calls;
    uint32_t worst_copy, worst_whole, worst_partial, worst_save, worst_notify;
    uint64_t shaped_words, reused_words;
} dvb_t;

// Both cores read the stream state: internal DRAM, 16-byte aligned (stream_assert.ld).
#ifdef __XTENSA__
#define STREAM_STATE __attribute__((section(".stream_bss"), aligned(16)))
#else
#define STREAM_STATE __attribute__((aligned(16)))
#endif
static STREAM_STATE dvb_t dvb;
// Only the lofty path uses this state. Dummy pairs preserve the four-symbol
// table rounding and vector-store alignment of every queued data descriptor.
typedef struct {
    uint32_t head[3] __attribute__((aligned(16))); // two descriptors; unused prefetch holds min lead
    uint32_t page_max;
    uint32_t raw[2], dummy_at, admission_pages;
    uint32_t *dummy;
    uint64_t output;
    uint32_t published, samples, frames, at, length;
    uint32_t startup, steady;
    unsigned skip, head_at, head_left;
    uint16_t history, dummy_history;
    bool filler, splice;
    uint32_t streaming, rate_misses, admission_gap, admission_max;
    struct { uint32_t words, cycles; } handoff[2];
    uint32_t queue_free_before, queue_free_after, queue_min_before;
    uint32_t queue_ready;
    bool queue_auto;
    uint32_t first_dummy_frame;
    unsigned resident_limit;
    uint32_t render_warm_cycles;
} lofty_mux_t;
// The table, 54 pages and mux fill the existing store exactly.
#define LOFTY_MUX_AT (4*LOFTY_TABLE_WORDS+LOFTY_STORED*sizeof(lofty_page))
#define lofty_mux (*(lofty_mux_t *)((uint8_t *)shaper_store+LOFTY_MUX_AT))
_Static_assert(LOFTY_MUX_AT+sizeof(lofty_mux_t) == sizeof shaper_store, "table, 54 pages and mux fill the store");
#define LOFTY_INPUT_BYTES ((sizeof shaper_store-LOFTY_MUX_AT-sizeof(lofty_mux_t)) & ~(size_t)15)
#define lofty_input ((uint8_t *)(&lofty_mux+1))
#define LOFTY_DUMMY_STRIDE 1672 // pair + two-symbol prefix + prefetch guard, aligned
#define LOFTY_STARTUP_FRAMES 3
#define LOFTY_BANK_SAMPLES RADIO_S3_STEP_WORDS_80M
#define LOFTY_BANK_CYCLES (3*LOFTY_BANK_SAMPLES)
#define LOFTY_PAGE_CYCLES (23*LOFTY_BLOCKS) // single-issue lower bound, renderer trace
#define LOFTY_MAX_PAGES 54
#define LOFTY_HEAP_FLOOR 20000u
#define LOFTY_HEAP_GUARD 2048u
#define LOFTY_RESERVE_PAGES 9 // 4606 symbols, including the two-symbol prefix
_Static_assert(LOFTY_BLOCKS == MACHINE_PAGE_BLOCKS,
               "both queues span the same number of samples per page");
_Static_assert(MACHINE_PAGE_BLOCKS%16 == 0 && sizeof(machine_page)%16 == 0,
               "queue pages retain prepare and render alignment");
_Static_assert(MACHINE_PAGES*5*MACHINE_PAGE_BLOCKS >= 2*33282,
               "fractional startup holds two complete pilot-on frames");
_Static_assert(sizeof(dvbs2_workspace) - DVBS2_FAST_WORKSPACE_USED >=
               SHAPER_HISTORY + BANK_SYMBOLS + 1, "fast workspace tail holds n4 window");
typedef struct {
    const dvbs2_mode *mode;
    dvbs2_fast *fast;               // fast encoder context for this mode, internal RAM
    dvbs2_workspace *ws;            // internal for fast QPSK / 4-sps 8PSK, otherwise PSRAM
    uint8_t *tables;               // moon: complete resident encoder data
    uint8_t *bch;                  // resident byte table with internal workspace, t = 10/12
    bool bch_borrowed;
    uint8_t *pair_bch;
    unsigned priority;
    bool ws_borrowed;
    bool quads;                     // four QPSK codes/byte, block shaper
    bool packed;                    // all fast rates: packed internal ring
    uint8_t *payload;               // packed: encoder scratch until BB consumes it; raw: PSRAM
    uint8_t *input;                 // pace reference: lofty PSRAM, QPSK internal
    uint32_t input_prepare_cycles;
    bool input_borrowed;
    union {
        uint8_t *slot[PACKED_SLOTS]; // legacy packed frames, internal
        bool dummy_locked;           // queued 26m667 uses pages instead of frame slots
    };
    uint8_t *block;                 // packed ring's heap part: slot 2 (4 sps) or everything (8 sps)
    uint8_t *slots, *filler;        // n8 8PSK dummy borrows PSRAM workspace tail
    TaskHandle_t task;
    uint32_t stall_ms;
    volatile bool stop;
    bool failed;
    volatile bool input_started;
    uint32_t input_waits, input_partial, input_pending, prefill_frames;
    uint32_t warmup_frames, warmup_cycles;
    uint32_t input_frame_cycles_max;
    SemaphoreHandle_t exited;
    uint32_t frames;                // encoded and published
    uint8_t *table_tail;            // lofty: program/columns/CRC in PSRAM; uses alignment padding
    uint64_t cycles;                // per frame: payload and encoding, raw
    uint32_t min_cycles, max_cycles, live_max_cycles; // live excludes the initial ring prefill
    uint64_t kernel_cycles;        // same-frame interval after payload generation
    uint32_t kernel_live_max;
    uint64_t work_cycles;          // kernel interval minus full-queue backpressure
    uint32_t work_live_max;
    uint64_t frame_work_cycles;     // measured input copy + kernel, excluding backpressure
    uint32_t frame_work_live_max;
    stream_profile_t stage[7];      // payload, BB, BCH, LDPC, map, PL, queue pages
    uint32_t slot_waits;
    uint32_t stack_min_free;
} encoder_t;

static STREAM_STATE encoder_t enc;
static struct {
    bool vcm, vcm_axes;
    unsigned requested, vcm_changes, mode_frames[14], vcm_modulations;
    void *vcm_heap[8];
    unsigned vcm_allocations;
    size_t vcm_store_bytes;
    uint32_t vcm_free_before, vcm_largest_before;
    uint64_t boundary_cycles;
    uint32_t boundary_max;
    uint32_t lengths[RING_MAX_SLOTS];
} vcm_enc VCM_BSS;
// The synchronous console configures the next run. The encoder alone owns
// requested/current mode while running, including in-band control callbacks.
static struct { bool enabled, mixed; unsigned hold, count, pattern[16]; } vcm VCM_BSS;

void dvbtx_vcm_command(const char *args)
{
    if (!strcmp(args,"off") || !strcmp(args,"on") || !strcmp(args,"mixed")) {
        memset(&vcm,0,sizeof vcm);
        vcm.enabled = strcmp(args,"off") != 0;
        vcm.mixed = !strcmp(args,"mixed");
    } else {
        unsigned pattern[16], count=0;
        char *end;
        const unsigned long hold = strtoul(args,&end,10);
        if (end == args || hold < 1 || hold > 65535) goto invalid;
        while (*end) {
            const char *at=end;
            const unsigned long mc=strtoul(at,&end,10);
            if (end == at || mc < 4 || mc > 17 || count == 16) goto invalid;
            pattern[count++]=(unsigned)mc;
        }
        if (!count) goto invalid;
        vcm.enabled=true; vcm.hold=(unsigned)hold; vcm.count=count;
        vcm.mixed=false;
        for (unsigned i=1;i<count;++i)
            if ((pattern[i] < 12) != (pattern[0] < 12)) vcm.mixed=true;
        memcpy(vcm.pattern,pattern,count*sizeof *pattern);
    }
    printf("@{\"cmd\":\"vcm\",\"enabled\":%s,\"mixed\":%s,\"hold_frames\":%u,\"pattern\":[",vcm.enabled?"true":"false",vcm.mixed?"true":"false",vcm.hold);
    for (unsigned i=0;i<vcm.count;++i) printf("%s%u",i?",":"",vcm.pattern[i]);
    printf("]}\n");
    return;
invalid:
    printf("@{\"cmd\":\"vcm\",\"error\":\"usage: vcm off|on|mixed|<1..65535 frames per mode> <MODCOD 4..17 ...> (up to 16)\"}\n");
}

static bool vcm_control(unsigned mc)
{
    if (!vcm_enc.vcm || vcm.count || mc < 4 || mc > 17 ||
        !(vcm_enc.vcm_modulations & (1u << dvbs2_find_mode(mc)->bits_per_symbol))) return false;
    vcm_enc.requested=mc;
    return true;
}

static int vcm_boundary(encoder_t *e, uint32_t frame)
{
    const unsigned mc=vcm.count ? vcm.pattern[(frame/vcm.hold)%vcm.count] : vcm_enc.requested;
    if (mc == e->mode->modcod) return 0;
    const dvbs2_mode *mode=dvbs2_find_mode(mc);
    // Reprepare only before reading this frame's payload: prepare clears scratch.
    // No allocation or pulse rebuild occurs while either core is streaming.
    if (dvbs2_fast_prepare(e->fast,mode,PILOTS,15 | DVBS2_VCM | DVBS2_BCH_NIBBLE |
                           (dvb.table.n == 8 ? DVBS2_MAP_SPARSE : 0))) return -1;
    e->mode=mode; vcm_enc.vcm_changes++;
    return 0;
}

static inline __attribute__((always_inline)) lofty_page *lofty_page_at(unsigned i)
{
    return dvb.lofty_pages+i;
}

static StaticSemaphore_t exited_storage;
static uint32_t cache_ranges[4];
static bool cache_configured;

static void cache_profile_start(void)
{
    // Count the actual cached address apertures (TRM v1.8 table 4.3-2, p405).
    // Do not clear shared counters, touch cache contents or change cache policy.
    const uint32_t regs[] = {EXTMEM_IBUS_TO_FLASH_START_VADDR_REG,
        EXTMEM_IBUS_TO_FLASH_END_VADDR_REG, EXTMEM_DBUS_TO_FLASH_START_VADDR_REG,
        EXTMEM_DBUS_TO_FLASH_END_VADDR_REG};
    const uint32_t ranges[] = {0x42000000, 0x43ffffff, 0x3c000000, 0x3dffffff};
    for (unsigned i = 0; i < 4; ++i) {
        cache_ranges[i] = REG_READ(regs[i]);
        REG_WRITE(regs[i], ranges[i]);
    }
    cache_configured = true;
}

static uint32_t next_byte(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x & 0xff;
}

enum { QUEUE_FEC = 1, QUEUE_MAP, QUEUE_WAIT, QUEUE_PL };
static void moon_wait(void)
{
    const uint32_t before = esp_cpu_get_cycle_count();
    const uint32_t get = __atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE);
    const bool live = get != 0;
    __atomic_store_n(&dvb.wait_put,dvb.produced,__ATOMIC_RELAXED);
    __atomic_store_n(&dvb.wait_get,get,__ATOMIC_RELAXED);
    __atomic_store_n(&dvb.producer_phase,QUEUE_WAIT,__ATOMIC_RELEASE);
    // Arm before rechecking space. A release before this store is observed by
    // the recheck; a later bank-end notification remains pending until taken.
    __atomic_store_n(&dvb.waiting,1,__ATOMIC_RELEASE);
    if (!enc.stop && dvb.produced-__atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE) == dvb.queue_pages)
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    __atomic_store_n(&dvb.waiting,0,__ATOMIC_RELEASE);
    const uint32_t cycles = esp_cpu_get_cycle_count()-before;
    dvb.queue_wait_cycles += cycles;
    if (cycles > dvb.queue_wait_max) dvb.queue_wait_max = cycles;
    if (live && !enc.stop && cycles > dvb.queue_wait_live_max) dvb.queue_wait_live_max = cycles;
    __atomic_store_n(&dvb.producer_phase,QUEUE_MAP,__ATOMIC_RELEASE);
}

#include "lofty_producer.inc"

static IRAM_ATTR void moon_wake(void)
{
    if (__atomic_exchange_n(&dvb.waiting,0,__ATOMIC_ACQ_REL)) {
        ++dvb.queue_wakes;
        const uint32_t before = esp_cpu_get_cycle_count();
        xTaskNotifyGive(enc.task);
        const uint32_t cycles = esp_cpu_get_cycle_count()-before;
        dvb.wake_bank += cycles;
    }
}

static IRAM_ATTR esp_err_t moon_shape(uint32_t *dst, uint64_t first, size_t count)
{
    if (first != dvb.queue_sample) return ESP_FAIL;
    while (count) {
        const uint32_t get = dvb.consumed;
        const uint32_t put = __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE);
        const uint32_t ready = put-get;
        if (ready < dvb.queue_low) dvb.queue_low = ready;
        const int32_t lead = ((int32_t)(ready*MACHINE_PAGE_SAMPLES)-(int32_t)dvb.page_sample)*(dvb.lofty ? 4 : 5)/12;
        if (lead < dvb.ring.min_lead) dvb.ring.min_lead = lead;
        if (!ready) {
            ++dvb.queue_empty;
            dvb.queue_fault.put = put;
            dvb.queue_fault.get = get;
            dvb.queue_fault.phase = __atomic_load_n(&dvb.producer_phase,__ATOMIC_ACQUIRE);
            dvb.queue_fault.wait_put = __atomic_load_n(&dvb.wait_put,__ATOMIC_RELAXED);
            dvb.queue_fault.wait_get = __atomic_load_n(&dvb.wait_get,__ATOMIC_RELAXED);
            return ESP_FAIL;
        }
        machine_page *page = dvb.pages ? dvb.pages+dvb.get_index : NULL;
        const size_t left = MACHINE_PAGE_SAMPLES-dvb.page_sample;
        const size_t take = count < left ? count : left;
        machine_axis_samples(dst,page->axes,dvb.axis_history,dvb.page_sample,take,
                             shaper_store,dvb.render_scratch);
        dst += take, count -= take, dvb.page_sample += take;
        dvb.queue_sample += take;
        if (dvb.page_sample == MACHINE_PAGE_SAMPLES) {
            dvb.page_sample = 0;
            dvb.axis_history = page->axes[MACHINE_PAGE_AXIS_WORDS-1];
            if (++dvb.get_index == dvb.queue_pages) dvb.get_index = 0;
            __atomic_store_n(&dvb.consumed,get+1,__ATOMIC_RELEASE);
            if (put-get-1 <= dvb.queue_pages-dvb.wake_pages) {
                dvb.wake_pending = true;
            }
        }
    }
    return ESP_OK;
}

static bool lofty_dummy_init(void)
{
    lofty_mux.dummy = heap_caps_aligned_alloc(32,2*LOFTY_DUMMY_STRIDE*4+sizeof(radio_s3_startup_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!lofty_mux.dummy) return false;
    memset(lofty_mux.dummy,0,2*LOFTY_DUMMY_STRIDE*4+sizeof(radio_s3_startup_t));
    // Setup only, before the encoder owns this workspace. No internal buffer.
    uint8_t *phases = enc.ws->bits;
    dvbs2_dummy(phases);
    for (unsigned prefix = 0; prefix <= 2; prefix += 2) {
        uint32_t h = 0, *dst = lofty_mux.dummy+(prefix ? LOFTY_DUMMY_STRIDE : 0);
        for (unsigned k = 0; k < 2*DVBS2_DUMMY_SYMBOLS+prefix; k += 4) {
            for (unsigned j = 0; j < 4; ++j) {
                unsigned at = k+j-prefix;
                h = (h<<3) | (at < 2*DVBS2_DUMMY_SYMBOLS ? phases[at%DVBS2_DUMMY_SYMBOLS] : 0);
            }
            *dst++ = lofty_descriptor(h);
        }
    }
    // Only head[0..2] feed the splice renderer. Keep both raw dummy heads
    // in its unused tail, so source selection never fetches PSRAM.
    uint32_t head = 0;
    for (unsigned k = 0; k < 8; ++k) head = (head<<3)|phases[k];
    lofty_mux.raw[0] = head;
    lofty_mux.raw[1] = ((head>>18)<<12) | ((head>>6)&4095);
    for (unsigned k = DVBS2_DUMMY_SYMBOLS-4; k < DVBS2_DUMMY_SYMBOLS; ++k)
        lofty_mux.dummy_history = (lofty_mux.dummy_history<<3) | phases[k];
    return true;
}

#include "lofty_consumer.inc"

// The synthetic payload is one xorshift32 byte stream. xorshift32 is linear over GF(2), so a
// state K steps ahead is M^K x for the 32x32 matrix M^K, stored by columns. For one frame
// length, eight PIE lanes then generate eight contiguous K-byte blocks at once
// (payload_pie.S); the scalar loop keeps the remainder, other lengths and the host build.
static struct { size_t bytes; unsigned k; uint32_t jump[32]; } xorshift;

static uint32_t xorshift_step(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void xorshift_prepare(size_t bytes)
{
    xorshift.bytes = xorshift.k = 0;
    const unsigned k = (unsigned)(bytes/8) & ~3u;
    if (k < 64) return;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t v = 1u << i;
        for (unsigned s = 0; s < k; ++s) v = xorshift_step(v);
        xorshift.jump[i] = v;
    }
    xorshift.bytes = bytes;
    xorshift.k = k;
}

#ifdef __XTENSA__
void payload_xorshift8(uint8_t *dst, unsigned quads, uint32_t *lanes, unsigned stride);
#endif

// Amortize loop control across four bytes of the same synthetic reference stream.
static __attribute__((noinline)) void payload_fill(uint8_t *dst, size_t n, uint32_t *x)
{
    if (n && n == xorshift.bytes) {
        uint32_t lanes[8] __attribute__((aligned(16)));
        lanes[0] = *x;
        for (unsigned j = 1; j < 8; ++j) {
            uint32_t y = 0, v = lanes[j-1];
            for (unsigned i = 0; v; ++i, v >>= 1) if (v & 1) y ^= xorshift.jump[i];
            lanes[j] = y;
        }
#ifdef __XTENSA__
        payload_xorshift8(dst,xorshift.k/4,lanes,xorshift.k);
#else
        // The host runs the same lane decomposition one lane at a time.
        for (unsigned j = 0; j < 8; ++j)
            for (unsigned i = 0; i < xorshift.k; ++i)
                dst[j*xorshift.k+i] = (uint8_t)(lanes[j] = xorshift_step(lanes[j]));
#endif
        *x = lanes[7];
        dst += 8*xorshift.k, n -= 8*xorshift.k;
    }
    uint32_t state = *x;
    for (; n >= 4; n -= 4, dst += 4) {
        dst[0] = next_byte(&state);
        dst[1] = next_byte(&state);
        dst[2] = next_byte(&state);
        dst[3] = next_byte(&state);
    }
    for (size_t i = 0; i < n; ++i) dst[i] = next_byte(&state);
    *x = state;
}

static void synthetic_payload(encoder_t *e, size_t bytes, uint32_t *seed)
{
    if (e->input) {
        // Match real ingress copy granularity (PV_PAYLOAD_MAX = 1316 B).
        // DMA, CRC and descriptor handling are outside this copy fixture.
        const size_t prefix = dvb.lofty ? LOFTY_INPUT_BYTES : 0;
        if (prefix) memcpy(e->payload,lofty_input,prefix);
        for (size_t at = prefix; at < bytes; at += PV_PAYLOAD_MAX) {
            const size_t n = bytes-at < PV_PAYLOAD_MAX ? bytes-at : PV_PAYLOAD_MAX;
            memcpy(e->payload+at,e->input+at-prefix,n);
        }
    } else payload_fill(e->payload,bytes,seed);
    station_payload_header(e->payload,bytes,station_store_get());
}

// Legacy whole-symbol starts have n % sps == 0. Since sps = 4, 8 or 40 and
// 16380 are multiples of four, (n % 16380)*4 bytes is 16-byte aligned.
// At 8 sps 16380 % 8 == 4, so a bank wrap can split a symbol in half. The
// QPSK block wrapper uses aligned four-symbol boundaries (12/16 samples).
#define FILL_VCM 0
#define FILL_NAME fill
#define FILL_TEXT IRAM_ATTR
#define FILL_LOFTY dvb.lofty
#define FILL_MOON dvb.moon
#define FILL_PACKED enc.packed
#define FILL_QUADS enc.quads
#include "dvb_fill.inc"
#undef FILL_NAME
#undef FILL_TEXT
#undef FILL_LOFTY
#undef FILL_MOON
#undef FILL_PACKED
#undef FILL_QUADS

#undef FILL_VCM
#define FILL_VCM 1
#define FILL_NAME fill_vcm
#define FILL_TEXT VCM_TEXT
#define FILL_LOFTY 0
#define FILL_MOON 0
#define FILL_PACKED enc.packed
#define FILL_QUADS 0
#include "dvb_fill.inc"
#undef FILL_NAME
#undef FILL_TEXT
#undef FILL_LOFTY
#undef FILL_MOON
#undef FILL_PACKED
#undef FILL_QUADS
#undef FILL_VCM

static radio_s3_fill_fn encoder_fill(void) { return vcm_enc.vcm ? fill_vcm : fill; }

// Core 0: frame after frame into the ring, waiting while every slot is still needed.
typedef struct {
    stream_mark_t clock;
    uint64_t wait;
} encoder_mark_t;

static inline __attribute__((always_inline)) encoder_mark_t encoder_mark(void)
{
    encoder_mark_t m = {.clock=dvb.profile ? stream_mark() : stream_mark_cycles()};
    m.wait = dvb.queue_wait_cycles;
    return m;
}

static inline __attribute__((always_inline)) encoder_mark_t encoder_record(stream_profile_t *p, encoder_mark_t before)
{
    const encoder_mark_t after = encoder_mark();
    stream_mark_t active = before.clock;
    active.cycles += (uint32_t)(after.wait-before.wait);
    stream_record(p,active,after.clock);
    return after;
}

#define ENCODER_TASK encoder_task
#define ENCODER_VCM 0
#include "dvb_encoder.inc"
#undef ENCODER_TASK
#undef ENCODER_VCM
#define ENCODER_TASK encoder_task_vcm
#define ENCODER_VCM 1
#include "dvb_encoder.inc"
#undef ENCODER_TASK
#undef ENCODER_VCM

static void release(void)
{
    spi_input_control(NULL);
    spi_input_buffers(NULL, 0, NULL, 0);
    if (cache_configured) {
        const uint32_t regs[] = {EXTMEM_IBUS_TO_FLASH_START_VADDR_REG,
            EXTMEM_IBUS_TO_FLASH_END_VADDR_REG, EXTMEM_DBUS_TO_FLASH_START_VADDR_REG,
            EXTMEM_DBUS_TO_FLASH_END_VADDR_REG};
        for (unsigned i = 0; i < 4; ++i) REG_WRITE(regs[i], cache_ranges[i]);
        cache_configured = false;
    }
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    if (vcm_enc.vcm) {
        for (unsigned i=0;i<vcm_enc.vcm_allocations;++i) heap_caps_free(vcm_enc.vcm_heap[i]);
        vcm_enc.vcm_allocations=0;
        enc.fast=NULL; enc.ws=NULL; enc.payload=enc.slots=enc.filler=enc.block=NULL;
        dvb.samples=NULL; dvb.win=NULL; enc.task=NULL;
        return;
    }
    heap_caps_free(dvb.samples);
    dvb.samples = NULL;
    heap_caps_free(enc.tables);
    heap_caps_free(enc.table_tail);
    heap_caps_free(enc.pair_bch);
    if (dvb.lofty) {
        if (enc.dummy_locked) {
            lofty_dummy_resident(lofty_mux.dummy,2*LOFTY_DUMMY_STRIDE*4,0);
            enc.dummy_locked = false;
        }
        heap_caps_free(lofty_mux.dummy); lofty_mux.dummy = NULL;
        lofty_resident(0);
    }
    heap_caps_free(enc.fast);
    if (!enc.input_borrowed) heap_caps_free(enc.input);
    enc.input = NULL;
    if (!enc.ws_borrowed) heap_caps_free(enc.ws);
    if (dvb.win && !dvb.moon && (vcm_enc.vcm ? !enc.ws_borrowed :
            (!enc.packed || (enc.mode->bits_per_symbol == 3 && dvb.table.n != 4))))
        heap_caps_free(dvb.win);
    dvb.win = NULL;
    if (!enc.bch_borrowed) heap_caps_free(enc.bch);
    heap_caps_free(enc.block);
    if (!enc.packed) {
        heap_caps_free(enc.payload);
        heap_caps_free(enc.slots);
        heap_caps_free(enc.filler);
    }
    enc.tables = enc.table_tail = enc.pair_bch = NULL;
    enc.fast = NULL, enc.ws = NULL, enc.bch = enc.block = enc.payload = enc.slots = enc.filler = NULL;
    enc.task = NULL;
}

// Bytes for a packed part, 16-byte aligned, with the 16 spare bytes the vector unpack may read.
static size_t packed_bytes(size_t symbols)
{
    return (symbols / 2 + 16 + 15) & ~(size_t)15;
}

// Report the failed request before cleanup, when the heap still reflects the
// attempted layout. Total free memory alone misses fragmentation.
static void vcm_allocation_error(const char *cmd, const char *stage, size_t bytes, unsigned caps)
{
    const unsigned internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    const unsigned external=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    printf("@{\"cmd\":\"%s\",\"error\":\"VCM allocation failed\",\"stage\":\"%s\","
           "\"requested_bytes\":%u,\"memory\":\"%s\",\"free_internal_before\":%"PRIu32","
           "\"largest_internal_before\":%"PRIu32",\"free_internal\":%u,\"largest_internal\":%u,"
           "\"free_psram\":%u,\"largest_psram\":%u,\"store_bytes\":%u}\n",
           cmd,stage,(unsigned)bytes,(caps&MALLOC_CAP_SPIRAM)?"psram":"internal",
           vcm_enc.vcm_free_before,vcm_enc.vcm_largest_before,
           (unsigned)heap_caps_get_free_size(internal),(unsigned)heap_caps_get_largest_free_block(internal),
           (unsigned)heap_caps_get_free_size(external),(unsigned)heap_caps_get_largest_free_block(external),
           (unsigned)vcm_enc.vcm_store_bytes);
}

static void *vcm_allocate(const char *cmd, const char *stage, size_t bytes, unsigned caps, bool borrow)
{
    const size_t at=(vcm_enc.vcm_store_bytes+15)&~(size_t)15;
    if (borrow && at+bytes <= sizeof shaper_store) {
        vcm_enc.vcm_store_bytes=at+bytes;
        return (uint8_t *)shaper_store+at;
    }
    void *p=heap_caps_aligned_alloc(16,bytes,caps);
    if (!p) { vcm_allocation_error(cmd,stage,bytes,caps); return NULL; }
    vcm_enc.vcm_heap[vcm_enc.vcm_allocations++]=p;
    return p;
}

// Render state lives in the existing static store wherever possible. FEC's
// maximum context is in PSRAM, avoiding a 17,696-byte internal allocation.
// Same-modulation patterns retain their phase table. Mixed 5m uses the compact
// five-level axis table, keeping both frames and all render state internal.
static __attribute__((noinline)) bool vcm_memory(const char *cmd, const dvbs2_mode *mode, unsigned sps, unsigned level)
{
    const unsigned internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    const unsigned external=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    vcm_enc.vcm=true; vcm_enc.requested=mode->modcod; enc.packed=sps != 40;
    vcm_enc.vcm_modulations=vcm.mixed ? 12 : 1u<<mode->bits_per_symbol;
    vcm_enc.vcm_axes=vcm.mixed; // mixed runs only at 5m (8 sps)
    vcm_enc.vcm_free_before=heap_caps_get_free_size(internal);
    vcm_enc.vcm_largest_before=heap_caps_get_largest_free_block(internal);
    const unsigned bits=vcm_enc.vcm_modulations == 4 ? 2 : 3;
    vcm_enc.vcm_store_bytes=vcm_enc.vcm_axes ? SHAPER_AXES_BYTES : 4*shaper_table_words(sps,bits);
    dvb.win_bytes=SHAPER_HISTORY+(RADIO_S3_BANK_WORDS+sps-1)/sps+2;
    dvb.win=vcm_allocate(cmd,"window",dvb.win_bytes,internal,true);
    if (!dvb.win) return false;
    if (enc.packed) {
        dvb.samples=vcm_allocate(cmd,"overlap",dvb.cached_words*4,internal,true);
        if (!dvb.samples) return false;
    }
    enc.filler=vcm_allocate(cmd,"dummy",enc.packed ? packed_bytes(DVBS2_DUMMY_SYMBOLS) : DVBS2_DUMMY_SYMBOLS,external,true);
    if (!enc.filler) return false;
    enc.ws=vcm_allocate(cmd,"workspace",DVBS2_FAST_WORKSPACE_USED,external,true);
    if (!enc.ws) return false;
    enc.ws_borrowed=(uintptr_t)enc.ws >= (uintptr_t)shaper_store &&
                    (uintptr_t)enc.ws < (uintptr_t)(shaper_store+SHAPER_STORE_WORDS);
    enc.fast=vcm_allocate(cmd,"context",dvbs2_fast_size(),external,false);
    if (!enc.fast) return false;
    unsigned slots=RING_SLOTS;
    if (enc.packed) {
        enc.payload=dvbs2_fast_payload(enc.fast);
        const size_t symbols=vcm_enc.vcm_modulations == 8 ? 22194 : DVBS2_MAX_SYMBOLS;
        const size_t frame=packed_bytes(symbols);
        const size_t at=(vcm_enc.vcm_store_bytes+15)&~(size_t)15;
        slots=(sizeof shaper_store-at)/frame;
        if (slots>PACKED_SLOTS) slots=PACKED_SLOTS;
        if (slots>=2) {
            for (unsigned i=0;i<slots;++i) enc.slot[i]=vcm_allocate(cmd,"ring",frame,external,true);
        } else {
            slots=2;
            enc.block=vcm_allocate(cmd,"ring",slots*frame,external,false);
            if (!enc.block) return false;
            for (unsigned i=0;i<slots;++i) enc.slot[i]=enc.block+i*frame;
        }
    } else {
        enc.payload=vcm_allocate(cmd,"payload",(58192-80)/8,external,false);
        if (!enc.payload) return false;
        enc.slots=vcm_allocate(cmd,"ring",slots*DVBS2_MAX_SYMBOLS,external,false);
        if (!enc.slots) return false;
    }
    if (dvbs2_fast_prepare(enc.fast,mode,PILOTS,15|DVBS2_VCM|DVBS2_BCH_NIBBLE|
                          (sps == 8 ? DVBS2_MAP_SPARSE : 0))) {
        vcm_allocation_error(cmd,"prepare",0,external); return false;
    }
    // This runs on main's 3584-byte stack, with shaper_build's bias array
    // below it. Keep even the 1m pulse scratch out of that call chain.
    const size_t tap_bytes=9*sps*sizeof(int32_t);
    int32_t *taps=heap_caps_malloc(tap_bytes,external);
    if (!taps) { vcm_allocation_error(cmd,"pulse taps",tap_bytes,external); return false; }
    const int32_t *source=sps == 40 ? shaper_taps_qpsk_n40_a35 : shaper_taps_8psk_n8_a35;
    for (unsigned i=0;i<9*sps;++i) taps[i]=(int32_t)((double)source[i]*level/WORST_PEAK);
    if (vcm_enc.vcm_axes) shaper_axes_build(&dvb.table,shaper_store,taps);
    else shaper_build(&dvb.table,shaper_store,taps,sps,bits);
    heap_caps_free(taps);
    if (enc.packed) {
        dvbs2_dummy(enc.slot[1]); dvbs2_pack(enc.slot[1],enc.filler,DVBS2_DUMMY_SYMBOLS);
        ring_init_slots(&dvb.ring,enc.slot,slots,enc.filler,DVBS2_MAX_SYMBOLS);
        if (bits == 2 && !vcm_enc.vcm_axes) ring_packed_qpsk(&dvb.ring);
        else ring_packed_codes(&dvb.ring);
        ring_cache(&dvb.ring,dvb.overlap,SHAPER_HISTORY+2);
    } else {
        dvbs2_dummy(enc.filler);
        ring_init(&dvb.ring,enc.slots,slots,enc.filler,DVBS2_MAX_SYMBOLS);
    }
    ring_filler_length(&dvb.ring,DVBS2_DUMMY_SYMBOLS);
    spi_input_buffers(NULL,0,NULL,0);
    return true;
}

// Build the scaled table and the dummy frame, start the encoder on core 0 and wait until it has
// filled every slot. Returns the frame length in symbols, or 0 after printing an error for `cmd`.
static size_t start_encoder_options(const char *cmd, unsigned modcod, unsigned level, unsigned stall_ms,
                                   unsigned sps, uint32_t dac_rate, bool auto_pages, bool fixture)
{
    const dvbs2_mode *mode = tx_find_mode(modcod);
    const bool fractional = sps == DVB_SPS_12_5;
    const bool lofty = mode && mode->bits_per_symbol == 3 && sps == 3;
    const bool moon = fractional || lofty;
    const bool short_pulse = !moon && mode && mode->bits_per_symbol == 3 && dac_rate == RADIO_S3_DAC_RATE_80M;
    const bool quads = mode && mode->bits_per_symbol == 2 && dac_rate == RADIO_S3_DAC_RATE_80M;
    if (!mode || (fractional && mode->bits_per_symbol != 2) || !((dac_rate == RADIO_S3_DAC_RATE &&
                    (sps == 4 || sps == 8 || (sps == 40 && mode->bits_per_symbol == 2))) ||
                   (dac_rate == RADIO_S3_DAC_RATE_80M &&
                    (sps == 4 || sps == 3 || fractional)))) {
        printf("@{\"cmd\":\"%s\",\"error\":\"invalid MODCOD/sps\"}\n", cmd);
        return 0;
    }
    if (mode->short_frame && (vcm.enabled || !(sps == 40 || sps == 8) || dac_rate != RADIO_S3_DAC_RATE)) {
        printf("@{\"cmd\":\"%s\",\"error\":\"short frames need 1m QPSK or 5m, without VCM\"}\n", cmd);
        return 0;
    }
    if (vcm.enabled) {
        bool valid=dac_rate == RADIO_S3_DAC_RATE && spi_input_type() != PV_TS && !fixture;
        valid = valid && (sps == 8 || (sps == 40 && modcod < 12));
        if (vcm.count && vcm.pattern[0] != modcod) valid=false;
        for (unsigned i=0;i<vcm.count;++i) if (sps == 40 && vcm.pattern[i] >= 12) valid=false;
        if (vcm.mixed && sps != 8) valid=false;
        if (!valid) {
            printf("@{\"cmd\":\"%s\",\"error\":\"VCM needs GCS/synthetic, 1m QPSK or 5m; mixed needs 5m; initial MODCOD = first pattern entry\"}\n",cmd);
            return 0;
        }
    }
    const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
    const bool input = spi_input_type() != 0;
    if (fixture && (!moon || input)) {
        printf("@{\"cmd\":\"%s\",\"error\":\"fixture needs QPSK 33m333 or 8PSK 26m667 and payload input off\"}\n",cmd);
        return 0;
    }
    dvb.dac_rate = dac_rate;
    dvb.moon = moon;
    dvb.lofty = lofty;
    if (lofty) {
        memset(&lofty_mux,0,sizeof lofty_mux);
        lofty_mux.head[2] = UINT32_MAX;
        lofty_mux.first_dummy_frame = UINT32_MAX;
        lofty_mux.page_max = UINT32_MAX; // calibrate on the first queued frame
        lofty_mux.admission_gap = UINT32_MAX;
        lofty_mux.admission_pages = LOFTY_FRAME_PAGES | (LOFTY_FRAME_PAGES<<8);
        lofty_mux.queue_auto = auto_pages;
    }
    dvb.queue_pages = lofty ? (modcod == 17 ? 46 : modcod == 13 ? LOFTY_MAX_PAGES : LOFTY_PAGES) : MACHINE_PAGES;
    if (lofty && lofty_mux.queue_auto) dvb.queue_pages = 0; // fixed in radio_s3_stream_ready
    if (lofty) lofty_mux.resident_limit = dvb.queue_pages < LOFTY_STORED ? dvb.queue_pages : LOFTY_STORED;
    dvb.wake_pages = lofty ? LOFTY_WAKE : MACHINE_WAKE_PAGES;
    dvb.pages = NULL;
    dvb.lofty_pages = NULL;
    dvb.last_word = 0;
    dvb.den = fractional ? 5 : 1;
    dvb.span = lofty ? 5 : moon ? 7 : 9;
    dvb.history = moon ? (fractional ? 10 : 8) : SHAPER_HISTORY;
    dvb.cached_words = CACHED_FOR(dac_rate);
    // Timed lofty playback starts with station ID. Its initial 128-word lead
    // never reaches this cache; every later data bank re-reads only 640 words.
    // Raw dumps have no ID and retain the complete initial 768-word overlap.
    if (lofty && strcmp(cmd,"dvbdump") && station_store_get()[0])
        dvb.cached_words -= RADIO_S3_LEAD_WORDS_80M;
    (void)heap_caps_monitor_local_minimum_free_size_start();   // low-water mark of this command
    memset(&enc, 0, sizeof enc);
    enc.mode = mode;
    memset(&vcm_enc,0,sizeof vcm_enc);
    spi_input_control(NULL);
    memset(&dvb.copy, 0, sizeof dvb.copy);
    memset(&dvb.whole, 0, sizeof dvb.whole);
    memset(&dvb.partial, 0, sizeof dvb.partial);
    memset(&dvb.save, 0, sizeof dvb.save);
    memset(&dvb.notify, 0, sizeof dvb.notify);
    dvb.fill_words = dvb.fill_cycles = dvb.fill_calls = 0;
    dvb.bank_cycles = dvb.bank_whole = dvb.bank_calls = 0;
    dvb.max_bank_cycles = dvb.max_bank_whole = dvb.max_bank_calls = 0;
    dvb.worst_copy = dvb.worst_whole = dvb.worst_partial = 0;
    dvb.worst_save = dvb.worst_notify = 0;
    dvb.shaped_words = dvb.reused_words = 0;
    dvb.sample_end = dvb.sample_at = 0;
    dvb.wake_pending = false;
    dvb.profile = !strcmp(cmd, "dvbpace") || !strcmp(cmd, "dvbdump");
    // The fill phases of a timed run record cycles only; an untimed dump also reads the
    // shared cache counters. Encoder stages always read them.
    dvb.copy.counters = dvb.whole.counters = dvb.partial.counters = !strcmp(cmd, "dvbdump");
    dvb.save.counters = dvb.notify.counters = dvb.copy.counters;
    if (dvb.profile) cache_profile_start();
    enc.stall_ms = stall_ms;
    enc.min_cycles = UINT32_MAX;
    for (unsigned i = 0; i < 7; ++i) {
        enc.stage[i].counters = 1;
    }
    xorshift_prepare(0); // VCM and failed setups keep the scalar generator
    if (vcm.enabled) {
        if (!vcm_memory(cmd,mode,sps,level)) {
            release(); return 0;
        }
        goto encoder_ready;
    }
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t payload_bytes = (mode->kbch - 80) / 8;
    xorshift_prepare(payload_bytes);
    size_t store_used = shaper_table_words(sps, mode->bits_per_symbol) * 4;
    enc.packed = sps != 40;
    enc.quads = quads;
    dvb.table.n = sps;
    // Three slots cover the first cold refill after the opening ID. QPSK n3 keeps
    // two in the store and allocates the third through the existing path.
    // At 10 Msym/s the 5/6 encoder needs about 69% of a frame period; a two-frame model keeps
    // as little as 244 symbols of lead there, three frames keep 18,871 even at 95% (test_ring).
    const unsigned nslots = moon ? 0 : !enc.packed ? RING_SLOTS : sps <= 4 ? 3 : 2;
    dvbs2_workspace *moon_ws = NULL;
    if (moon) {
        dvb.produced = dvb.consumed = dvb.page_sample = dvb.waiting = 0;
        dvb.put_index = dvb.get_index = 0;
        dvb.queue_sample = 0;
        dvb.queue_low = dvb.queue_pages;
        dvb.queue_empty = dvb.queue_wakes = dvb.wake_max = dvb.wake_bank = 0;
        dvb.producer_phase = dvb.wait_put = dvb.wait_get = 0;
        memset(&dvb.queue_fault,0,sizeof dvb.queue_fault);
        dvb.prepare_max = 0;
        dvb.prepare_tail_max = 0;
        dvb.prepare_cycles = dvb.read_cycles = 0;
        dvb.prepare_pages = 0;
        if (lofty) {
            if (lofty_resident(1)) {
                printf("@{\"cmd\":\"%s\",\"error\":\"renderer code residency failed\"}\n",cmd);
                release(); return 0;
            }
            // Before the renderer table exists, borrow it to ask the public
            // encoder API for this mode's cache size. Reserve that large block
            // before the context and workspace can split the available heaps.
            dvbs2_fast *probe = (dvbs2_fast *)shaper_store;
            if (dvbs2_fast_mode_size(mode) > LOFTY_TABLE_WORDS*sizeof *shaper_store ||
                dvbs2_fast_prepare(probe,mode,PILOTS,15)) {
                printf("@{\"cmd\":\"%s\",\"error\":\"encoder cache sizing failed\"}\n",cmd);
                release(); return 0;
            }
            const size_t tail = dvbs2_fast_cache_tail_size(probe);
            const size_t bytes = dvbs2_fast_cache_size(probe)-tail+DVBS2_LOFTY_CACHE_BYTES;
            enc.tables = heap_caps_aligned_alloc(16,bytes,internal);
            if (enc.tables) enc.table_tail = heap_caps_aligned_alloc(16,tail,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if (!enc.tables || !enc.table_tail) {
                printf("@{\"cmd\":\"%s\",\"error\":\"encoder cache allocation failed\",\"modcod\":%u,"
                       "\"requested_bytes\":%u,\"free_internal\":%u,\"largest_internal\":%u}\n",
                       cmd,modcod,(unsigned)bytes,(unsigned)heap_caps_get_free_size(internal),
                       (unsigned)heap_caps_get_largest_free_block(internal));
                release(); return 0;
            }
            lofty_build(shaper_store,level); // discard the sizing context
        } else machine_build(shaper_store,level);
        dvb.page_blocks = dvb.page_words = 0;
        dvb.mapper = (dvbs2_sign_state){0};
        dvb.axis_history = 0;
        dvb.queue_wait_cycles = dvb.queue_wait_max = dvb.queue_wait_live_max = dvb.prepare_ticks = 0;
        dvb.page_clock = dvb.page_gap_max = dvb.page_wait = 0;
        uint8_t *tail;
        if (lofty) {
            dvb.lofty_pages = (lofty_page *)(shaper_store+LOFTY_TABLE_WORDS);
            tail = (uint8_t *)(dvb.lofty_pages+LOFTY_STORED);

        } else {
            dvb.pages = (machine_page *)(shaper_store+MACHINE_TABLE_WORDS);
            dvb.render_scratch = (uint32_t *)(dvb.pages+MACHINE_PAGES);
            moon_ws = (dvbs2_workspace *)(dvb.render_scratch+MACHINE_AXIS_CHUNK_SAMPLES);
            tail = (uint8_t *)moon_ws+DVBS2_FAST_WORKSPACE_USED;
        }
        dvb.win = NULL;
        dvb.win_bytes = 0;
        store_used = tail-(uint8_t *)shaper_store;
        if (lofty) store_used = sizeof shaper_store;
        if (store_used > sizeof shaper_store) { release(); return 0; }
    } else if (quads) {
        const size_t frame = ((symbols+3)/4 + 31) & ~(size_t)15;
        const size_t dummy = ((DVBS2_DUMMY_SYMBOLS+3)/4 + 31) & ~(size_t)15;
        const size_t used = QPSK_TABLE_WORDS(sps)*4;
        const unsigned stored = sps == 3 ? 2 : 0;
        uint8_t *base = (uint8_t *)shaper_store + used;
        if (stored < nslots) enc.block = heap_caps_aligned_alloc(16, (nslots-stored)*frame, internal);
        for (unsigned i = 0; i < nslots; ++i)
            enc.slot[i] = i < stored ? base+i*frame : enc.block ? enc.block+(i-stored)*frame : NULL;
        // n4 lends the table tail to the workspace instead of one frame:
        // 12,420 B reclaimed rather than 8,352 B, saving another 4,068 heap bytes.
        const size_t extra = sps == 4 ? (sizeof *enc.ws+15) & ~(size_t)15 : stored*frame;
        enc.filler = base+extra;
        store_used = used+extra+dummy;
        if (used+extra+dummy > sizeof shaper_store) {
            release();
            return 0;
        }
    } else if (enc.packed) {
        const size_t dummy = packed_bytes(DVBS2_DUMMY_SYMBOLS);
        // The dummy frame is built unpacked in the second slot: a short 8PSK frame packs into less.
        const size_t frame = packed_bytes(symbols) > DVBS2_DUMMY_SYMBOLS ? packed_bytes(symbols) : (DVBS2_DUMMY_SYMBOLS + 15) & ~(size_t)15;
        const size_t used = short_pulse ? 16384 : shaper_table_words(sps, mode->bits_per_symbol) * 4;
        const size_t need = 2 * frame + dummy;
        uint8_t *base;
        if (mode->bits_per_symbol == 2) {
            // QPSK's 4/8 KiB table leaves room for all frames, dummy and payload.
            const size_t total = nslots * frame + dummy;
            store_used = used+total;
            if (used + total > sizeof shaper_store) {
                printf("@{\"cmd\":\"%s\",\"error\":\"internal ring does not fit\"}\n", cmd);
                release();
                return 0;
            }
            base = (uint8_t *)shaper_store + used;
            for (unsigned i = 0; i < nslots; ++i) enc.slot[i] = base + i * frame;
            enc.filler = base + nslots * frame;
            enc.payload = enc.filler + dummy;
        } else {
            // Preserve the 8PSK layout and allocation order.
            if (sps == 4) {
                const size_t total = used + ((need + 15) & ~(size_t)15) + (short_pulse ? frame : 0);
                if (total > sizeof shaper_store) {
                    printf("@{\"cmd\":\"%s\",\"error\":\"internal ring does not fit\"}\n", cmd);
                    release();
                    return 0;
                }
                base = (uint8_t *)shaper_store + used;
                store_used = total;
                if (short_pulse) enc.slot[2] = base + ((need + 15) & ~(size_t)15);
                else {
                    enc.slot[2] = enc.block = heap_caps_aligned_alloc(16, frame, internal);
                    if (!enc.block) base = NULL;
                }
            } else {
                // The slow n8 workspace is in PSRAM and has an unused fast-stage
                // tail. Keep the rarely read dummy there, leaving frames internal.
                base = enc.block = heap_caps_aligned_alloc(16, 2*frame, internal);
            }
            if (base) {
                enc.slot[0] = base;
                enc.slot[1] = base + frame;
                enc.filler = base + 2 * frame;
                enc.payload = enc.filler + dummy;
            }
        }
    }
    // Fast QPSK keeps its workspace internal. The demo must retain byte BCH
    // with input too: changing to nibble BCH is not an input-copy cost.
    const bool resident = enc.packed && (moon || mode->bits_per_symbol == 2 || sps == 4);
    const bool input_bch = mode->bits_per_symbol == 3 && sps == 4 &&
                           (short_pulse || modcod == 13) && mode->t > 8;
    if (input_bch) {
        enc.bch = (uint8_t *)shaper_store + store_used;
        enc.bch_borrowed = true;
        store_used += DVBS2_BCH_CACHE_BYTES;
    }
    // The slow 8-sps 8PSK path already uses cached PSRAM for its workspace.
    // Keep its context there too when SPI is selected, leaving internal RAM for the input slots.
    enc.fast = heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode),
                                      input && enc.packed && !resident ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : internal);
    enc.ws_borrowed = quads || (moon && !lofty);
    enc.ws = lofty ? heap_caps_aligned_alloc(16,DVBS2_FAST_WORKSPACE_USED,internal) :
        moon ? moon_ws : enc.ws_borrowed && sps == 4 ? (dvbs2_workspace *)(shaper_store + QPSK_TABLE_WORDS(4)) :
        enc.ws_borrowed && sps == 3 ? (dvbs2_workspace *)((uint8_t *)shaper_store+store_used) :
        heap_caps_aligned_alloc(16, sizeof *enc.ws,
                               resident ? internal : MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (enc.packed && !resident)
        enc.filler = enc.ws ? (uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED : NULL;
    if (!moon) dvb.win_bytes = quads ? 2 + (RADIO_S3_BANK_WORDS + 8*sps-2)/(4*sps) + 1 :
                           SHAPER_HISTORY + (RADIO_S3_BANK_WORDS + sps - 1) / sps + 2;
    if (!moon && enc.ws_borrowed && sps == 3) store_used += DVBS2_FAST_WORKSPACE_USED;
    if (enc.packed) enc.payload = dvbs2_fast_payload(enc.fast);
    if (!moon) dvb.win = enc.ws_borrowed && sps == 3 ? (uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED : resident ? (enc.ws ? (uint8_t *)enc.ws + DVBS2_FAST_WORKSPACE_USED : NULL) :
                        heap_caps_aligned_alloc(16, dvb.win_bytes, internal);
    if (!moon && resident && mode->t > 8 && !input && !input_bch)
        enc.bch = heap_caps_aligned_alloc(16, DVBS2_BCH_CACHE_BYTES, internal);
    if (!enc.packed) {
        // QPSK: main's order and sizes, so the PSRAM addresses, and with them the data cache
        // sets that the fill's frame reads share with the encoder, stay as they were.
        enc.payload = heap_caps_malloc(payload_bytes, MALLOC_CAP_SPIRAM);
        enc.slots = heap_caps_malloc(RING_SLOTS * symbols, MALLOC_CAP_SPIRAM);
        enc.filler = heap_caps_malloc(DVBS2_DUMMY_SYMBOLS, MALLOC_CAP_SPIRAM);
    }
    int32_t *taps = heap_caps_malloc(TAPS * sizeof *taps, MALLOC_CAP_SPIRAM);
    // Only resident tables use byte BCH. Flash byte tables lost in the stream
    // (results/2026-10-16-next); the other modes keep register-nibble BCH.
    // At 8-sps 8PSK the PSRAM workspace needs the sparse mapper's smaller
    // cache footprint. Its temporary phases reuse the existing context scratch.
    if ((!moon && (!dvb.win || !enc.filler)) || !enc.fast || !enc.ws || !enc.payload || !taps ||
            (!moon && resident && mode->t > 8 && !input && !enc.bch) || (!enc.packed && !enc.slots) ||
            (!moon && quads && !enc.slot[nslots-1]) ||
            dvbs2_fast_prepare(enc.fast, mode, PILOTS,
                              15 | (fractional ? DVBS2_RO_20 : 0) | (quads ? DVBS2_MAP_QUADS : 0) |
                              (input && !input_bch && !lofty ? DVBS2_BCH_NIBBLE : 0) |
                              (!moon && mode->t == 8 && mode->bits_per_symbol == 2 && !quads ? DVBS2_BCH_NIBBLE : 0) |
                              (resident ? 0 : (mode->t == 8 && !input ? 0 : DVBS2_BCH_NIBBLE) | DVBS2_MAP_SPARSE)) ||
            (enc.bch && dvbs2_fast_cache_bch(enc.fast, enc.bch))) {
        printf("@{\"cmd\":\"%s\",\"error\":\"allocation failed\"}\n", cmd);
        heap_caps_free(taps);
        release();
        return 0;
    }
    if (moon) {
        const size_t cache = dvbs2_fast_cache_size(enc.fast);
        const size_t heap = cache-(lofty ? dvbs2_fast_cache_tail_size(enc.fast) : 0);
        if (!lofty) enc.tables = heap_caps_aligned_alloc(16,cache,internal);
        if (!enc.tables || dvbs2_fast_cache_split(enc.fast,enc.tables,enc.table_tail) ||
            (lofty && dvbs2_fast_lofty_cache(enc.fast,(uint32_t *)(enc.tables+heap)))) {
            heap_caps_free(taps); release();
            printf("@{\"cmd\":\"%s\",\"error\":\"encoder cache allocation failed\"}\n",cmd);
            return 0;
        }
    }
    if (lofty && mode->t > 8) {
        // T1 is read every two bytes: keep it internal, beside T0.
        enc.pair_bch = heap_caps_aligned_alloc(16,DVBS2_BCH_PAIR_BYTES,internal);
        if (!enc.pair_bch || dvbs2_fast_cache_bch_pair(enc.fast,enc.pair_bch)) {
            printf("@{\"cmd\":\"%s\",\"error\":\"BCH pair allocation failed\"}\n",cmd);
            heap_caps_free(taps); release(); return 0;
        }
    }
    if (lofty && !lofty_dummy_init()) {
        printf("@{\"cmd\":\"%s\",\"error\":\"dummy allocation failed\"}\n",cmd);
        heap_caps_free(taps); release(); return 0;
    }
    if (enc.packed) {
        dvb.samples = heap_caps_aligned_alloc(16,dvb.cached_words*sizeof *dvb.samples,internal);
        if (!dvb.samples) {
            heap_caps_free(taps);
            release();
            printf("@{\"cmd\":\"%s\",\"error\":\"overlap allocation failed\"}\n",cmd);
            return 0;
        }
    }
    if (enc.packed && !moon) {
        // Pack the dummy PLFRAME through the second slot, before the encoder owns it.
        dvbs2_dummy(enc.slot[1]);
        if (quads) dvbs2_pack_quads(enc.slot[1], enc.filler, DVBS2_DUMMY_SYMBOLS);
        else dvbs2_pack(enc.slot[1], enc.filler, DVBS2_DUMMY_SYMBOLS);
    } else if (!moon) {
        dvbs2_dummy(enc.filler);
    }
    // Scale the conservative envelope to level; table rounding adds at most 1.5 LSB/axis.
    const int32_t *source = sps == 40 ? shaper_taps_qpsk_n40_a35 :
                            sps == 8 ? shaper_taps_8psk_n8_a35 : shaper_taps_8psk_n4_a35;
    for (unsigned i = 0; !moon && !quads && i < 9 * sps; ++i) {
        taps[i] = (int32_t)((double)source[i] * level / WORST_PEAK);
    }
    if (moon) dvb.table = (shaper_table_t){.blocks=shaper_store, .n=sps, .bits=mode->bits_per_symbol};
    else if (quads) {
        shaper_qpsk_build(shaper_store, sps, level);
        dvb.table = (shaper_table_t){.blocks=shaper_store, .n=sps, .bits=2};
    } else if (short_pulse) shaper_short_build(&dvb.table, shaper_store, level);
    else shaper_build(&dvb.table, shaper_store, taps, sps, mode->bits_per_symbol);
    heap_caps_free(taps);
    dvb.max_fill_cycles = 0;
    if (enc.packed) {
        ring_init_slots(&dvb.ring, enc.slot, nslots, enc.filler, symbols);
        if (!moon) ring_cache(&dvb.ring, dvb.overlap, SHAPER_HISTORY+2);
        if (quads) dvb.ring.packed = 4;
        else if (!moon && mode->bits_per_symbol == 2) ring_packed_qpsk(&dvb.ring);
        else ring_packed_codes(&dvb.ring);
    } else {
        ring_init(&dvb.ring, enc.slots, RING_SLOTS, enc.filler, symbols);
    }
    ring_filler_length(&dvb.ring, DVBS2_DUMMY_SYMBOLS);

    if (moon && !input && fixture) {
        const size_t at = (store_used+15) & ~(size_t)15;
        enc.input_borrowed = at+payload_bytes <= sizeof shaper_store;
        enc.input = enc.input_borrowed ? (uint8_t *)shaper_store+at :
                    heap_caps_malloc(payload_bytes-(lofty ? LOFTY_INPUT_BYTES : 0),
                                     lofty ? MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT : internal);
        if (!enc.input) {
            release();
            printf("@{\"cmd\":\"%s\",\"error\":\"reference input allocation failed\"}\n",cmd);
            return 0;
        }
        uint32_t x = SEED;
        const uint32_t before = esp_cpu_get_cycle_count();
        const size_t prefix = lofty ? LOFTY_INPUT_BYTES : 0;
        if (prefix) payload_fill(lofty_input,prefix,&x);
        payload_fill(enc.input,payload_bytes-prefix,&x);
        enc.input_prepare_cycles = esp_cpu_get_cycle_count()-before;
        if (enc.input_borrowed) store_used = at+payload_bytes;
    }
    if (enc.ws_borrowed && sps == 3) store_used += dvb.win_bytes;
    const size_t tail = (store_used+15) & ~(size_t)15;
    const size_t ws_used = (DVBS2_FAST_WORKSPACE_USED+dvb.win_bytes+15) & ~(size_t)15;
    spi_input_buffers(!moon && tail < sizeof shaper_store ? (uint8_t *)shaper_store+tail : NULL,
                      !moon && tail < sizeof shaper_store ? sizeof shaper_store-tail : 0,
                      resident && !moon && !(enc.ws_borrowed && sps == 3) ? (uint8_t *)enc.ws+ws_used : NULL,
                      resident && !moon && !(enc.ws_borrowed && sps == 3) && ws_used < sizeof *enc.ws ? sizeof *enc.ws-ws_used : 0);

encoder_ready:;
    const unsigned ready_slots=dvb.ring.nslots;
    // Bounded bursts, then sleep on the full queue. Startup and
    // stop need no controller boost because a full producer always blocks.
    enc.priority = moon ? MOON_PRIORITY : ENCODER_PRIORITY;
    enc.exited = xSemaphoreCreateBinaryStatic(&exited_storage);
    // Measured 8PSK stack use stayed below 1072 bytes at all three keyed rates.
    if (xTaskCreatePinnedToCore(vcm_enc.vcm ? encoder_task_vcm : encoder_task, "encoder", lofty ? ENCODER_STACK-256 : ENCODER_STACK, &enc, enc.priority, &enc.task, 0)
            != pdPASS) {
        if (vcm_enc.vcm) vcm_allocation_error(cmd,"encoder task",ENCODER_STACK,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        else printf("@{\"cmd\":\"%s\",\"error\":\"encoder task failed\"}\n", cmd);
        release();
        return 0;
    }
    if (lofty && lofty_mux.queue_auto) return symbols;
    const bool external = spi_input_type() != 0;
    const TickType_t limit = xTaskGetTickCount() + pdMS_TO_TICKS(external ? 100 : (moon ? 3 : ready_slots) * (stall_ms + 100));
    while ((moon ? __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed < dvb.queue_pages :
            __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE) < ready_slots) && !enc.failed &&
           (int32_t)(xTaskGetTickCount()-limit) < 0) vTaskDelay(1);
    if (enc.failed || (external ? !enc.input_started : moon ? dvb.produced-dvb.consumed < dvb.queue_pages :
            __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE) < ready_slots)) {
        enc.stop = true;
        xTaskNotifyGive(enc.task);
        xSemaphoreTake(enc.exited, portMAX_DELAY);
        vTaskDelete(enc.task);
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder did not fill the queue\"}\n",cmd);
        release();
        return 0;
    }
    enc.prefill_frames = moon ? enc.frames : __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE);
    return symbols;
}

// Where a table lives: internal SRAM (data bus), PSRAM or flash, both through the cache.
static const char *memory_class(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    if (!p) return "none";
    if (a >= 0x3fc88000u && a < 0x3fd00000u) return "internal";
    return esp_ptr_external_ram(p) ? "psram" : "flash";
}

// Stop the encoder after a run and print the stream part of the result record.
static void print_profile(const char *name, const stream_profile_t *p)
{
    printf("\"%s\":{\"calls\":%" PRIu32 ",\"cycles\":%" PRIu64 ",\"max_cycles\":%" PRIu32
           ",\"flash_miss\":%" PRIu64 ",\"psram_miss\":%" PRIu64
           ",\"data_access\":%" PRIu64 ",\"instruction_miss\":%" PRIu64 ",\"prepare_cycles\":%" PRIu64 "}",
           name, p->calls, p->cycles, p->max_cycles, p->flash_miss, p->psram_miss,
           p->data_access, p->instruction_miss, p->prepare_cycles);
}

static size_t start_encoder(const char *cmd, unsigned modcod, unsigned level, unsigned stall_ms,
                            unsigned sps, uint32_t dac_rate)
{
    return start_encoder_options(cmd,modcod,level,stall_ms,sps,dac_rate,
                                 !strcmp(cmd,"dvb"),false);
}

static void print_queue_region(unsigned first, unsigned pages, const lofty_page *p)
{
    const uintptr_t begin = (uintptr_t)p, end = begin+pages*sizeof *p;
    int low = -1, high = -1;
#ifdef __XTENSA__
    // IDF v6.1 esp32s3/memory_layout.c:90-99, levels 2 through 9.
    if (begin >= 0x3fc88000u && end <= 0x3fcf8000u) {
        low = (begin-0x3fc80000u)/65536+2;
        high = (end-1-0x3fc80000u)/65536+2;
    }
#endif
    printf("{\"first_page\":%u,\"pages\":%u,\"begin\":\"0x%"PRIxPTR"\",\"end\":\"0x%"PRIxPTR
           "\",\"sram_levels\":[%d,%d],\"dac_overlap\":%s}",first,pages,begin,end,low,high,
           begin < 0x3fce0000u && end > 0x3fcc0000u ? "true" : "false");
}

static void lofty_warm_renderer(void)
{
    // The overlap cache is invalid until the first fill. Borrow it without
    // changing sample_end, page ownership, frame history or profiling counters.
    const uint32_t before = esp_cpu_get_cycle_count();
    const unsigned chunk = dvb.cached_words/12;
    for (unsigned variant = 0; variant < 2; ++variant) {
        const uint32_t *src = lofty_mux.dummy+variant*LOFTY_DUMMY_STRIDE;
        const unsigned words = (2*DVBS2_DUMMY_SYMBOLS+2*variant+3)/4;
        for (unsigned at = 0; at < words;) {
            unsigned n = words-at < chunk ? words-at : chunk;
            lofty_samples(dvb.samples,src+at,0,12*n,shaper_store);
            at += n;
        }
    }
    lofty_samples(dvb.samples+1,lofty_mux.dummy,1,11,shaper_store);
    lofty_mux.render_warm_cycles = esp_cpu_get_cycle_count()-before;
}

esp_err_t radio_s3_stream_ready(void)
{
    if (!enc.task || !dvb.lofty) return ESP_OK;
    // Setup may have suspended/invalidated cache since prepare.
    if (lofty_resident(0) || lofty_resident(1)) return ESP_FAIL;
    if (!lofty_mux.queue_auto || lofty_mux.queue_ready) return ESP_OK;
    const TickType_t started = xTaskGetTickCount()+pdMS_TO_TICKS(100);
    while (!enc.input_started && !enc.failed && (int32_t)(xTaskGetTickCount()-started) < 0) vTaskDelay(1);
    if (!enc.input_started || enc.failed) return ESP_FAIL;
    const unsigned caps = MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    const size_t free = heap_caps_get_free_size(caps);
    lofty_mux.queue_free_before = free;
    lofty_mux.queue_min_before = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (lofty_mux.queue_min_before < LOFTY_HEAP_FLOOR) return ESP_ERR_NO_MEM;
    // The queue is static; keep a heap floor and guard for the run.
    if (free < LOFTY_HEAP_FLOOR+LOFTY_HEAP_GUARD) return ESP_ERR_NO_MEM;
    const unsigned pages = LOFTY_MAX_PAGES;
    lofty_mux.queue_free_after = heap_caps_get_free_size(caps);
    dvb.queue_pages = dvb.queue_low = pages;
    lofty_mux.resident_limit = pages;
    __atomic_store_n(&lofty_mux.queue_ready,true,__ATOMIC_RELEASE);
    xTaskNotifyGive(enc.task);
    const TickType_t limit = xTaskGetTickCount()+pdMS_TO_TICKS(3*(enc.stall_ms+100));
    while (__atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed < pages && !enc.failed &&
           (int32_t)(xTaskGetTickCount()-limit) < 0) vTaskDelay(1);
    if (enc.failed || dvb.produced-dvb.consumed < pages) return ESP_FAIL;
    enc.prefill_frames = enc.frames;
    lofty_warm_renderer();
    // The ID lasts seconds. A warm-at-setup PSRAM buffer can be evicted before
    // the first data/dummy boundary, which also starts a cold encoder burst.
    // Keep both immutable dummy variants resident for this session, without
    // taking internal heap or changing any renderer, sample or queue choice.
    if (lofty_dummy_resident(lofty_mux.dummy,2*LOFTY_DUMMY_STRIDE*4,1)) return ESP_FAIL;
    enc.dummy_locked = true;
    return ESP_OK;
}

static void stop_encoder(void)
{
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    enc.stop = true;
    xTaskNotifyGive(enc.task);
    xSemaphoreTake(enc.exited, portMAX_DELAY);
    vTaskDelete(enc.task);
    if (vcm_enc.vcm) {
        printf(",\"vcm_axes\":%s",vcm_enc.vcm_axes ? "true" : "false");
        printf(",\"vcm\":true,\"vcm_changes\":%u,\"last_modcod\":%u,"
               "\"vcm_boundary_cycles_mean\":%"PRIu64",\"vcm_boundary_cycles_max\":%u,\"mode_frames\":[",
               vcm_enc.vcm_changes,enc.mode->modcod,enc.frames ? vcm_enc.boundary_cycles/enc.frames : 0,(unsigned)vcm_enc.boundary_max);
        for (unsigned i=0;i<14;++i) printf("%s%u",i?",":"",vcm_enc.mode_frames[i]);
        printf("],\"vcm_scope\":\"%s\",\"vcm_store_bytes\":%u,\"vcm_table_bytes\":%u,"
               "\"free_internal_before\":%"PRIu32",\"largest_internal_before\":%"PRIu32,
               vcm_enc.vcm_modulations == 12 ? "mixed" : vcm_enc.vcm_modulations == 4 ? "qpsk" : "8psk",
               (unsigned)vcm_enc.vcm_store_bytes,(unsigned)(vcm_enc.vcm_axes ? SHAPER_AXES_BYTES : 4*shaper_table_words(dvb.table.n,dvb.table.bits)),
               vcm_enc.vcm_free_before,vcm_enc.vcm_largest_before);
    }
    ring_t *r = &dvb.ring;
    if (dvb.lofty) {
        r->min_lead = lofty_mux.head[2] == UINT32_MAX ? INT64_MAX : lofty_mux.head[2]/3;
        r->next_frame = lofty_mux.frames;
        r->next_position = lofty_mux.frames+lofty_mux.startup+lofty_mux.steady;
    } else if (dvb.moon) {
        r->next_frame = dvb.queue_sample*(dvb.lofty ? 4 : 5)/(12*r->frame_symbols);
        r->next_position = r->next_frame;
    }
    spi_input_print();
    printf(",\"fast_memory\":\"%s\"", (vcm_enc.vcm || (spi_input_type() && enc.mode->bits_per_symbol == 3 && dvb.table.n == 8)) ? "psram" : "internal");
    printf(",\"input_pending\":%" PRIu32, enc.input_pending);
    printf(",\"input_waits\":%" PRIu32 ",\"input_partial\":%" PRIu32
           ",\"input_frame_cycles_max\":%" PRIu32,
           enc.input_waits, enc.input_partial, enc.input_frame_cycles_max);
    printf(",\"ring_slots\":%d,\"stall_ms\":%" PRIu32 ",\"frames_encoded\":%" PRIu32 ","
           "\"frames_played\":%" PRIu32 ",\"positions\":%" PRIu32 ",\"underruns\":%" PRIu32 ","
           "\"min_lead_symbols\":%lld,\"encode_cycles_mean\":%" PRIu64 ","
           "\"encode_cycles_min\":%" PRIu32 ",\"encode_cycles_max\":%" PRIu32 ","
           "\"encode_cycles_live_max\":%" PRIu32 ","
           "\"encode_kernel_cycles_mean\":%" PRIu64 ",\"encode_kernel_cycles_live_max\":%" PRIu32 ",\"prefill_frames\":%u,"
           "\"encoder_failed\":%s,\"max_fill_call_cycles\":%" PRIu32 ",\"free_internal\":%u,"
           "\"largest_internal\":%u,\"min_free_internal\":%u",
           (int)r->nslots, enc.stall_ms, enc.frames, dvb.fill_calls ? r->next_frame : 0, dvb.fill_calls ? r->next_position : 0, r->underruns,
           r->min_lead == INT64_MAX ? -1LL : (long long)r->min_lead,
           enc.frames ? enc.cycles / enc.frames : 0, enc.frames ? enc.min_cycles : 0,
           enc.max_cycles, enc.live_max_cycles,
           enc.frames ? enc.kernel_cycles / enc.frames : 0, enc.kernel_live_max, (unsigned)enc.prefill_frames,
           enc.failed ? "true" : "false", dvb.max_fill_cycles,
           (unsigned)internal, (unsigned)largest, (unsigned)lowest);
    printf(",\"callsign\":\"%s\",\"encoder_timed\":true,\"sps\":%.8g,\"sps_num\":%u,\"sps_den\":%u,\"fill_kernel\":\"%s\"",
           station_store_get(), (double)dvb.table.n/dvb.den, dvb.table.n, dvb.den,
           dvb.lofty ? "lofty-queue" : dvb.moon ? "moon-queue" : vcm_enc.vcm_axes ? "pie-axes5" : "pie");
    printf(",\"payload_source\":\"%s\",\"input_frames\":%u,\"input_memory\":\"%s\",\"input_prepare_cycles\":%" PRIu32
           ",\"encoder_priority\":%u,\"encoder_pie\":%u,\"encode_frame_work_cycles_mean\":%" PRIu64 ",\"encode_frame_work_cycles_live_max\":%" PRIu32,
           enc.input ? (dvb.lofty ? "psram-frame" : "internal-frame") : spi_input_type() ? spi_input_source() : "xorshift32",enc.input ? 1 : 0,
           enc.input ? (dvb.lofty ? "psram" : "internal") : "none",enc.input_prepare_cycles,enc.priority,dvbs2_fast_pie(enc.fast),
           enc.frames ? enc.frame_work_cycles/enc.frames : 0,enc.frame_work_live_max);
    if (dvb.moon) printf(",\"queue_wait_cycles\":%" PRIu64 ",\"queue_wait_max_cycles\":%u,\"queue_wait_live_max_cycles\":%u,\"prepare_max_ticks\":%u,\"queue_page_gap_max_cycles\":%u,\"queue_page_samples\":%u,\"queue_wake_pages\":%u",
                         dvb.queue_wait_cycles,(unsigned)dvb.queue_wait_max,(unsigned)dvb.queue_wait_live_max,(unsigned)dvb.prepare_ticks,(unsigned)dvb.page_gap_max,MACHINE_PAGE_SAMPLES,dvb.wake_pages);
    if (dvb.moon) printf(",\"queue_wake_policy\":\"bank\"");
    if (dvb.lofty) printf(",\"dummy_startup\":%u,\"dummy_steady\":%u,\"dummy_alignment\":%u,\"admission_pages\":%u,\"admission_wait_pages\":%u,\"dummy_startup_frames\":%u,\"queue_stream_page_max_cycles\":%u,\"queue_stream_bound_misses\":%u,\"streaming_admissions\":%u,\"admission_update_max_cycles\":%u",
                          (unsigned)lofty_mux.startup,(unsigned)lofty_mux.steady,(unsigned)((lofty_mux.startup+lofty_mux.steady)/2),
                          (unsigned)(lofty_mux.admission_pages&255),(unsigned)(lofty_mux.admission_pages>>8),
                          LOFTY_STARTUP_FRAMES,(unsigned)lofty_mux.page_max,(unsigned)lofty_mux.rate_misses,(unsigned)lofty_mux.streaming,(unsigned)lofty_mux.admission_max);
    if (dvb.lofty) printf(",\"dummy_startup_frame\":%"PRIi32,(int32_t)lofty_mux.first_dummy_frame);
    if (dvb.lofty) printf(",\"render_warm_cycles\":%"PRIu32,lofty_mux.render_warm_cycles);
    if (dvb.lofty) printf(",\"renderer_icache_bytes\":%u",(unsigned)lofty_resident_bytes());
    if (dvb.lofty) printf(",\"dummy_dcache_bytes\":%u",enc.dummy_locked ? 2*LOFTY_DUMMY_STRIDE*4 : 0);
    if (dvb.moon && dvb.queue_empty) printf(",\"queue_fault\":{\"produced\":%"PRIu32",\"consumed\":%"PRIu32
        ",\"producer_phase\":%"PRIu32",\"last_wait_produced\":%"PRIu32",\"last_wait_consumed\":%"PRIu32"}",
        dvb.queue_fault.put,dvb.queue_fault.get,dvb.queue_fault.phase,
        dvb.queue_fault.wait_put,dvb.queue_fault.wait_get);
    if (dvb.moon) printf(",\"bch_pair_bytes\":%u",enc.pair_bch ? DVBS2_BCH_PAIR_BYTES : 0);
    if (dvb.lofty) {
        printf(",\"queue_sizing\":{\"automatic\":%s,\"ready\":%s,\"target_pages\":%u,"
               "\"floor_bytes\":%u,\"guard_bytes\":%u,\"free_before\":%u,\"free_after\":%u,\"min_before\":%u},"
               "\"reference_psram_bytes\":%u,\"reference_store_bytes\":%u",
               lofty_mux.queue_auto ? "true" : "false",lofty_mux.queue_ready ? "true" : "false",
               LOFTY_MAX_PAGES,LOFTY_HEAP_FLOOR,LOFTY_HEAP_GUARD,
               (unsigned)lofty_mux.queue_free_before,(unsigned)lofty_mux.queue_free_after,(unsigned)lofty_mux.queue_min_before,
               enc.input ? (unsigned)heap_caps_get_allocated_size(enc.input) : 0,
               enc.input ? (unsigned)LOFTY_INPUT_BYTES : 0);
        printf(",\"queue_layout\":{\"contiguous\":%s,\"page_bytes\":%u,\"resident_pages\":%u,\"regions\":[",
               dvb.queue_pages <= LOFTY_STORED ? "true" : "false",(unsigned)sizeof(lofty_page),LOFTY_STORED);
        if (dvb.queue_pages) print_queue_region(0,dvb.queue_pages < LOFTY_STORED ? dvb.queue_pages : LOFTY_STORED,dvb.lofty_pages);
        printf("]}");
        printf(",\"handoff\":[");
        for (unsigned i=0; i<2; ++i)
            printf("%s{\"words\":%u,\"shape_cycles\":%u}",i ? "," : "",
                   (unsigned)lofty_mux.handoff[i].words,(unsigned)lofty_mux.handoff[i].cycles);
        printf("]");
    }
    if (dvb.moon) printf(",\"warmup_frames\":%u,\"warmup_cycles\":%u",(unsigned)enc.warmup_frames,(unsigned)enc.warmup_cycles);
    if (dvb.moon) printf(",\"encode_work_cycles_mean\":%" PRIu64 ",\"encode_work_cycles_live_max\":%" PRIu32,
                         enc.frames ? enc.work_cycles/enc.frames : 0,enc.work_live_max);
    if (dvb.moon && !dvb.lofty) printf(",\"history_expansion\":\"fused\",\"payload_bank_max_cycles\":%u,"
                         "\"payload_bank_max_whole_cycles\":%u,\"payload_bank_max_calls\":%u",
                         (unsigned)dvb.max_bank_cycles,(unsigned)dvb.max_bank_whole,(unsigned)dvb.max_bank_calls);
    if (dvb.moon) printf(",\"queue_pages\":%u,\"queue_low_pages\":%u,\"queue_empty\":%u,"
                         "\"queue_wake_max_cycles\":%u,\"queue_wakes\":%u,\"prepare_pages\":%u,"
                         "\"prepare_max_cycles\":%u,\"prepare_tail_max_cycles\":%u,\"prepare_cycles\":%" PRIu64 ",\"page_read_cycles\":%" PRIu64 ","
                         "\"table_bytes\":%u,\"table_heap_bytes\":0,\"encoder_cache_bytes\":%u",
                         dvb.queue_pages,(unsigned)dvb.queue_low,(unsigned)dvb.queue_empty,(unsigned)dvb.wake_max,(unsigned)dvb.queue_wakes,
                         (unsigned)dvb.prepare_pages,(unsigned)dvb.prepare_max,(unsigned)dvb.prepare_tail_max,dvb.prepare_cycles,dvb.read_cycles,(dvb.lofty ? LOFTY_TABLE_WORDS : MACHINE_TABLE_WORDS)*4,
                         (unsigned)(dvbs2_fast_cache_size(enc.fast)+(dvb.lofty ? DVBS2_LOFTY_CACHE_BYTES : 0)));
    if (dvb.moon) {
        const size_t heap = heap_caps_get_allocated_size(enc.tables);
        printf(",\"encoder_cache_heap_bytes\":%u,\"encoder_cache_store_bytes\":0,\"encoder_cache_psram_bytes\":%u",
               (unsigned)heap,enc.table_tail ? (unsigned)dvbs2_fast_cache_tail_size(enc.fast) : 0);
    }
    const void *bch_table, *bch_pair;
    const char *bch_variant = dvbs2_fast_bch_variant(enc.fast,&bch_table,&bch_pair);
    printf(",\"bch_table_memory\":\"%s\",\"bch_pair_memory\":\"%s\"",
           memory_class(bch_table),memory_class(bch_pair));
    printf(",\"workspace_memory\":\"%s\",\"workspace_bytes\":%u,\"fast_bytes\":%u,"
           "\"workspace_heap_bytes\":%u,\"fast_heap_bytes\":%u,\"bch_cache_heap_bytes\":%u,"
           "\"bch_cache_bytes\":%u,\"bch_variant\":\"%s\",\"packed_mapper\":\"%s\"",
           (vcm_enc.vcm ? enc.ws_borrowed : enc.packed && (dvb.moon || enc.mode->bits_per_symbol == 2 || dvb.table.n == 4)) ? "internal" : "psram",
           (unsigned)(dvb.lofty || vcm_enc.vcm ? DVBS2_FAST_WORKSPACE_USED : sizeof *enc.ws),
           (unsigned)(vcm_enc.vcm ? dvbs2_fast_size() : dvbs2_fast_mode_size(enc.mode)),
           enc.ws_borrowed ? 0 : (unsigned)heap_caps_get_allocated_size(enc.ws),
           (unsigned)heap_caps_get_allocated_size(enc.fast),
           enc.bch && !enc.bch_borrowed ? (unsigned)heap_caps_get_allocated_size(enc.bch) : 0,
           enc.bch || enc.tables ? DVBS2_BCH_CACHE_BYTES : 0, bch_variant,
           !enc.packed ? "none" : dvb.lofty ? "words" : dvb.moon ? "signs" : enc.quads ? "quads" : enc.mode->bits_per_symbol == 2 || dvb.table.n == 4 ? "direct" : "sparse");
    printf(",\"encoder_stack_bytes\":%u,\"encoder_stack_min_free\":%u", dvb.lofty ? ENCODER_STACK-256 : ENCODER_STACK, (unsigned)enc.stack_min_free);
    printf(",\"ring_memory\":\"%s\",\"ring_format\":\"%s\",\"ring_heap_bytes\":%u,"
           "\"payload_memory\":\"%s\",\"slot_waits\":%" PRIu32,
           (vcm_enc.vcm ? enc.packed && !enc.block : enc.packed) ? "internal" : "psram", dvb.lofty ? "words" : dvb.moon ? "signs" : enc.quads ? "quads" : enc.packed ? "packed" : "raw",
           enc.block ? (unsigned)heap_caps_get_allocated_size(enc.block) : 0,
           enc.packed && !vcm_enc.vcm ? "internal" : "psram", enc.slot_waits);
    printf(",\"layout\":{\"table\":\"0x%" PRIxPTR "\",\"fast\":\"0x%" PRIxPTR
           "\",\"workspace\":\"0x%" PRIxPTR "\",\"payload\":\"0x%" PRIxPTR
           "\",\"filler\":\"0x%" PRIxPTR "\",\"bch\":\"0x%" PRIxPTR
           "\",\"encoder_tables\":\"0x%" PRIxPTR "\",\"slots\":[",
           (uintptr_t)dvb.table.blocks, (uintptr_t)enc.fast, (uintptr_t)enc.ws,
           (uintptr_t)enc.payload, (uintptr_t)r->filler, (uintptr_t)enc.bch,
           (uintptr_t)enc.tables);
    for (unsigned i = 0; i < r->nslots; ++i) {
        printf(i ? ",\"0x%" PRIxPTR "\"" : "\"0x%" PRIxPTR "\"", (uintptr_t)r->slots[i]);
    }
    printf("]");
    if (dvb.lofty) printf(",\"reference\":\"0x%"PRIxPTR"\",\"dummy\":\"0x%"PRIxPTR"\",\"encoder_cache_tail\":\"0x%"PRIxPTR"\"",
                          (uintptr_t)enc.input,(uintptr_t)lofty_mux.dummy,(uintptr_t)enc.table_tail);
    printf("}");
    if (dvb.profile) {
        printf(",\"profile\":{\"cache_scope\":\"both cores and interrupts; windows overlap\","
               "\"fill_counters\":%s,\"fill_phase_cycles\":%s,\"fill_calls\":%" PRIu32 ",\"fill_words\":%" PRIu64
               ",\"fill_cycles\":%" PRIu64 ",\"worst_call\":{\"copy\":%" PRIu32 ",\"whole\":%" PRIu32
               ",\"partial\":%" PRIu32 ",\"save\":%" PRIu32 ",\"notify\":%" PRIu32 "},"
               "\"shaped_words\":%" PRIu64 ",\"reused_words\":%" PRIu64 ",\"fill\":{",
               dvb.copy.counters ? "true" : "false", !dvb.lofty || dvb.copy.counters ? "true" : "false",
               dvb.fill_calls, dvb.fill_words,
               dvb.fill_cycles, dvb.worst_copy, dvb.worst_whole, dvb.worst_partial,
               dvb.worst_save, dvb.worst_notify, dvb.shaped_words, dvb.reused_words);
        print_profile("copy", &dvb.copy); printf(",");
        print_profile("whole", &dvb.whole); printf(",");
        print_profile("partial", &dvb.partial); printf(",");
        print_profile("save", &dvb.save); printf(",");
        print_profile("notify", &dvb.notify); printf("},\"encoder\":{");
        static const char *const names[] = {"payload", "bb", "bch", "ldpc", "map", "pl", "pages"};
        for (unsigned i = 0; i < (dvb.moon ? 7u : 6u); ++i) {
            if (i) printf(",");
            print_profile(names[i], &enc.stage[i]);
        }
        printf("}}");
    }
    release();
}

static void print_mode(const char *cmd, unsigned modcod, size_t symbols, unsigned level)
{
    const dvbs2_mode *mode = tx_find_mode(modcod);
    printf("@{\"cmd\":\"%s\",\"modcod\":%u,\"rate\":\"%s\",\"symbol_rate\":%u,\"rolloff\":%.2f,"
           "\"pilots\":%d,\"frame_symbols\":%u,\"payload_bytes_per_frame\":%u,\"peak_level\":%u,\"short\":%s",
           cmd, modcod, mode->rate, (unsigned)((uint64_t)dvb.dac_rate*dvb.den / dvb.table.n), dvb.moon && dvb.den == 5 ? .20 : .35, PILOTS, (unsigned)symbols,
           (unsigned)((mode->kbch - 80) / 8), level, mode->short_frame ? "true" : "false");
}

static void print_timing(const radio_s3_stats_t *s)
{
    printf(",\"fills\":%" PRIu32 ",\"switches\":%" PRIu32 ",\"late_fills\":%" PRIu32 ","
           "\"late_switches\":%" PRIu32 ",\"max_fill_cycles\":%" PRIu32 ","
           "\"min_fill_slack_cycles\":%" PRIu32 ",\"max_switch_late_cycles\":%" PRIu32 ","
           "\"fatal_late\":%" PRIu32 ",\"elapsed_samples\":%" PRIu64
           ",\"max_fill_sample\":%" PRIu64,
           s->fills, s->switches, s->late_fills, s->late_switches, s->max_fill_cycles,
           s->min_fill_slack_cycles, s->max_switch_late_cycles, s->fatal_late,
           s->elapsed_samples, s->max_fill_sample);
    printf(",\"id_count\":%" PRIu32 ",\"id_samples\":%" PRIu32
           ",\"planned_samples\":%" PRIu64, s->id_count, s->id_samples, s->planned_samples);
}

static radio_s3_startup_t *lofty_startup(void)
{
    return (radio_s3_startup_t *)(lofty_mux.dummy+2*LOFTY_DUMMY_STRIDE);
}

static void print_startup(void)
{
    if (!dvb.lofty) return;
    const radio_s3_startup_t *t = lofty_startup();
    printf(",\"startup\":{\"first_sample\":%"PRIu64",\"columns\":[\"fill_cycles\",\"psram_miss\",\"trace_cycles\"],\"banks\":[",t->first_sample);
    for (unsigned i = 0; i < t->count; ++i)
        printf("%s[%"PRIu32",%"PRIu32",%"PRIu32"]",i ? "," : "",
               t->bank[i].cycles,t->bank[i].psram_miss,t->bank[i].trace_cycles);
    printf("]}");
}

void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps, uint32_t dac_rate, bool fixture)
{
    if (!station_require("dvb")) {
        return;
    }
    const size_t symbols = start_encoder_options("dvb",modcod,level,0,sps,dac_rate,true,fixture);
    if (!symbols) {
        return;
    }
    radio_s3_stats_t s = {0};
    // RRC support B = Rs * (1 + alpha), alpha = 0.20 or 0.35. Round upward in integer Hz.
    const uint32_t bandwidth = ((uint64_t)dac_rate * dvb.den * (dvb.den == 5 ? 120 : 135) + sps * 100 - 1) / (sps * 100);
    const esp_err_t err = radio_s3_run_bandwidth(freq,(uint8_t)gain,seconds,dac_rate,bandwidth,encoder_fill(),NULL,&s);
    print_mode("dvb", modcod, symbols, level);
    printf(",\"prepared_fixture\":%s",fixture ? "true" : "false");
    printf(",\"freq_mhz\":%" PRIu32 ",\"seconds\":%" PRIu32 ",\"gain\":%u,\"dac_rate\":%" PRIu32
           ",\"step_words\":%u", freq, seconds, gain, dac_rate, RADIO_S3_STEP_FOR(dac_rate));
    printf(",\"pulse_symbols\":%u", dvb.moon ? dvb.span : dvb.table.parts == 2 ? 6 : 9);
    print_timing(&s);
    printf(",\"fe_gain_index\":%" PRIu32 ",\"chain\":\"cwgain\"", s.fe_gain_index);
    radio_filter_print(&s.filter);
    radio_s3_tuning_print(&s);
    stop_encoder();
    printf(",\"status\":\"%s\"%s}\n", esp_err_to_name(err),
           err == ESP_ERR_INVALID_ARG ? ",\"error\":\"RF frequency out of range or duration too short for start/end ID\"" :
           err != ESP_OK ? ",\"error\":\"radio run failed\"" : "");
}

void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps, uint32_t dac_rate, bool fixture)
{
    const size_t symbols = start_encoder_options("dvbpace",modcod,level,stall_ms,sps,dac_rate,true,fixture);
    if (!symbols) return;
    radio_s3_stats_t s;
    const radio_s3_options_t options = {.startup=dvb.lofty ? lofty_startup() : NULL};
    const esp_err_t err = radio_s3_paced_run(seconds,dac_rate,encoder_fill(),NULL,&options,&s);
    print_mode("dvbpace", modcod, symbols, level);
    printf(",\"prepared_fixture\":%s",fixture ? "true" : "false");
    printf(",\"seconds\":%" PRIu32 ",\"dac_rate\":%" PRIu32 ",\"step_words\":%u", seconds,
           dac_rate, RADIO_S3_STEP_FOR(dac_rate));
    printf(",\"pulse_symbols\":%u", dvb.moon ? dvb.span : dvb.table.parts == 2 ? 6 : 9);
    print_timing(&s);
    print_startup();
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
                unsigned stall_ms, unsigned sps, uint32_t dac_rate)
{
    const size_t symbols = start_encoder("dvbdump", modcod, level, stall_ms, sps, dac_rate);
    if (!symbols) {
        return;
    }
    uint64_t duration_symbols=(uint64_t)frames*symbols;
    if (vcm_enc.vcm && vcm.count) {
        duration_symbols=0;
        for (unsigned f=0;f<frames;++f)
            duration_symbols+=dvbs2_frame_symbols(dvbs2_find_mode(vcm.pattern[(f/vcm.hold)%vcm.count]),PILOTS);
    }
    const uint64_t samples = (duration_symbols * dvb.table.n+dvb.den-1)/dvb.den;
    const uint64_t bytes = (samples + decim - 1) / decim * 4;
    // The USB staging buffer lives in PSRAM: the sink runs between fills, never inside one.
    dump_t *d = heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM);
    if (!d) {
        print_mode("dvbdump", modcod, symbols, level);
        stop_encoder();
        printf(",\"status\":\"ESP_ERR_NO_MEM\",\"error\":\"allocation failed\"}\n");
        return;
    }
    d->decim = decim, d->used = 0, d->crc = 0;
    printf("@{\"cmd\":\"dvbdump\",\"binary_bytes\":%" PRIu64 ",\"format\":\"int16 I/Q LE\","
           "\"sample_rate\":%" PRIu32 "}\n", bytes, dac_rate / decim);
    fflush(stdout);
    radio_s3_dry_stats_t s;
    const esp_err_t err = radio_s3_dry_run_rate(encoder_fill(), NULL, samples, dac_rate, sink, d, &s);
    if (d->used) {
        flush(d);
    }
    const uint32_t crc = d->crc;
    heap_caps_free(d);
    print_mode("dvbdump", modcod, symbols, level);
    printf(",\"pulse_symbols\":%u", dvb.moon ? dvb.span : dvb.table.parts == 2 ? 6 : 9);
    printf(",\"frames\":%u,\"decimation\":%u,\"samples\":%" PRIu64 ",\"crc32\":%" PRIu32 ","
           "\"fills\":%" PRIu32 ",\"max_fill_cycles\":%" PRIu32 ",\"overlap_words\":%" PRIu32 ","
           "\"overlap_mismatches\":%" PRIu32,
           frames, decim, samples, crc, s.fills, s.max_fill_cycles, s.overlap_words,
           s.overlap_mismatches);
    stop_encoder();
    printf(",\"status\":\"%s\"}\n", esp_err_to_name(err));
}
