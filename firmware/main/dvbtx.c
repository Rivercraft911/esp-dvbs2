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
#include "station_id.h"
#include "station_store.h"
#include "station_commands.h"
#include "shaper.h"
#include "shaper_qpsk.h"
#include "shaper_taps.h"
#include "machine.h"
#include "lofty.h"
#include "stream_jitter.h"

#define RING_SLOTS 8                // legacy QPSK frames ahead: 233 ms at 1 Msym/s
#define PACKED_SLOTS 3              // fast rates: three packed frames at 4 sps, two at 8, plus a cache
#define PILOTS 1
#define BANK_SYMBOLS 4096           // ceil(16380/4) + 1, worst offset at 4 sps
#define TAPS (9 * 40)
#define WORST_PEAK 718.4            // conservative sqrt(2)*508 LSB axis bound, before rounding
#define SEED 2026
#define ENCODER_PRIORITY 5          // above the command loop, below the Wi-Fi and IPC tasks
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
    bool page_wake;                // diagnostic comparison with the original publication path
    unsigned queue_pages, wake_pages;
    lofty_page *lofty_pages;
    uint16_t last_word;
    machine_page *pages;           // borrowed shaper-store tail
    uint32_t produced, consumed;   // single writer each; release/acquire ownership
    unsigned put_index, get_index, page_blocks;
    dvbs2_sign_state mapper;
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
    bool wake_pending;             // legacy slot released in the first bank callback
    const uint32_t *sign_lut;
    uint32_t prepare_max;
    uint32_t prepare_tail_max;
    uint64_t prepare_cycles;
    uint32_t prepare_pages;
    uint32_t dac_rate;             // the run's bank geometry
    size_t cached_words;           // active sample cache length
    stream_profile_t copy, whole, partial, save, notify;
    uint64_t fill_words, fill_cycles;
    uint32_t fill_calls;
    uint32_t worst_copy, worst_whole, worst_partial, worst_save, worst_notify;
    uint64_t shaped_words, reused_words;
} dvb_t;

static DRAM_ATTR dvb_t dvb;
_Static_assert(LOFTY_BLOCKS == MACHINE_PAGE_BLOCKS && sizeof(lofty_page) == sizeof(machine_page),
               "both queues use the same page geometry");
_Static_assert(4*LOFTY_TABLE_WORDS+LOFTY_STORED*sizeof(lofty_page)+DVBS2_FAST_WORKSPACE_USED <= 65536,
               "lofty tables, resident pages and workspace fit the store");
_Static_assert(MACHINE_PAGE_BLOCKS%4 == 0 && sizeof(machine_page)%16 == 0,
               "queue pages retain prepare and render alignment");
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
    uint8_t *input;                 // new-mode pace: internal frame, QPSK borrows the unused table tail
    uint32_t input_prepare_cycles;
    bool input_borrowed;
    uint8_t *slot[PACKED_SLOTS];    // packed frames, internal
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
    uint64_t cycles;                // per frame: payload and encoding, raw
    uint32_t min_cycles, max_cycles, live_max_cycles; // live excludes the initial ring prefill
    uint64_t kernel_cycles;        // same-frame interval after payload generation
    uint32_t kernel_live_max;
    uint64_t work_cycles;          // kernel interval minus full-queue backpressure
    uint32_t work_live_max;
    uint64_t frame_work_cycles;     // measured input copy + kernel, excluding backpressure
    uint32_t frame_work_live_max;
    stream_profile_t stage[6];      // payload, BB, BCH, LDPC, map, PL
    uint32_t slot_waits;
    uint32_t stack_min_free;
} encoder_t;

static encoder_t enc;
static inline lofty_page *lofty_page_at(unsigned i)
{
    return i < LOFTY_STORED ? dvb.lofty_pages+i : (lofty_page *)enc.block+i-LOFTY_STORED;
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

static IRAM_ATTR __attribute__((noinline)) int moon_map(bool warmup)
{
    if ((dvb.lofty ? dvbs2_fast_lofty_start(enc.fast,enc.ws,&dvb.mapper) :
                     dvbs2_fast_sign_start(enc.fast,enc.ws,&dvb.mapper))) return -1;
    while (!enc.stop && dvb.mapper.at < dvb.ring.frame_symbols) {
        while (!warmup && !enc.stop && dvb.produced-__atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE) == dvb.queue_pages)
            moon_wait();
        if (enc.stop) return -1;
        machine_page *page = dvb.pages ? dvb.pages+dvb.put_index : NULL;
        lofty_page *lp = dvb.lofty ? lofty_page_at(dvb.put_index) : NULL;
        const TickType_t tick = xTaskGetTickCount();
        const uint32_t before = esp_cpu_get_cycle_count();
        const unsigned room = MACHINE_PAGE_BLOCKS-dvb.page_blocks;
        unsigned n = room;
        if (lp) {
            const uint16_t *words = dvbs2_fast_lofty_read(enc.fast,&dvb.mapper,&n);
            if (n) dvb.last_word = lofty_prepare(lp->words+dvb.page_blocks,words,n,dvb.last_word);
        } else n = dvbs2_fast_sign_read(enc.fast,&dvb.mapper,page->signs+dvb.page_blocks,room,dvb.sign_lut);
        // Warm the same mapper/prepare kernels without publishing or advancing
        // the stream. One owned queue page is enough for every discarded chunk.
        if (warmup) { dvb.page_blocks = 0; continue; }
        const uint32_t cycles = esp_cpu_get_cycle_count()-before;
        if (cycles > dvb.prepare_max) {
            dvb.prepare_max = cycles;
            dvb.prepare_ticks = xTaskGetTickCount()-tick;
        }
        dvb.prepare_cycles += cycles;
        dvb.page_blocks += n;
        if (dvb.page_blocks == MACHINE_PAGE_BLOCKS) {
            if (lp) lp->words[LOFTY_BLOCKS] = 0;
            else page->signs[MACHINE_PAGE_BLOCKS] = 0;
            const uint32_t tail = esp_cpu_get_cycle_count();
            if (dvb.prepare_pages) {
                const uint32_t gap = tail-dvb.page_clock-(uint32_t)(dvb.queue_wait_cycles-dvb.page_wait);
                if (gap > dvb.page_gap_max) dvb.page_gap_max = gap;
            }
            dvb.page_clock = tail;
            dvb.page_wait = dvb.queue_wait_cycles;
            dvb.page_blocks = 0;
            if (++dvb.put_index == dvb.queue_pages) dvb.put_index = 0;
            __atomic_store_n(&dvb.produced,dvb.produced+1,__ATOMIC_RELEASE);
            const uint32_t spent = esp_cpu_get_cycle_count()-tail;
            if (spent > dvb.prepare_tail_max) dvb.prepare_tail_max = spent;
            ++dvb.prepare_pages;
        }
    }
    return enc.stop ? -1 : 0;
}

static IRAM_ATTR void moon_wake(void)
{
    if (__atomic_exchange_n(&dvb.waiting,0,__ATOMIC_ACQ_REL)) {
        ++dvb.queue_wakes;
        const uint32_t before = esp_cpu_get_cycle_count();
        xTaskNotifyGive(enc.task);
        const uint32_t cycles = esp_cpu_get_cycle_count()-before;
        dvb.wake_bank += cycles;
        stream_jitter_notify(cycles);
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
        if (dvb.lofty) lofty_samples(dst,lofty_page_at(dvb.get_index)->words+dvb.page_sample/12,
                                     dvb.page_sample%12,take,shaper_store);
        else machine_samples(dst,page->signs+dvb.page_sample/12,dvb.page_sample%12,take,shaper_store);
        dst += take, count -= take, dvb.page_sample += take;
        dvb.queue_sample += take;
        if (dvb.page_sample == MACHINE_PAGE_SAMPLES) {
            dvb.page_sample = 0;
            if (++dvb.get_index == dvb.queue_pages) dvb.get_index = 0;
            __atomic_store_n(&dvb.consumed,get+1,__ATOMIC_RELEASE);
            if (put-get-1 <= dvb.queue_pages-dvb.wake_pages) {
                if (dvb.page_wake) moon_wake();
                else dvb.wake_pending = true;
            }
        }
    }
    return ESP_OK;
}

// Amortize loop control across four bytes of the same synthetic reference stream.
static __attribute__((noinline)) void payload_fill(uint8_t *dst, size_t n, uint32_t *x)
{
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
        for (size_t at = 0; at < bytes; at += PV_PAYLOAD_MAX) {
            const size_t n = bytes-at < PV_PAYLOAD_MAX ? bytes-at : PV_PAYLOAD_MAX;
            memcpy(e->payload+at,e->input+at,n);
        }
    } else payload_fill(e->payload,bytes,seed);
    station_payload_header(e->payload,bytes,station_store_get());
}

// Legacy whole-symbol starts have n % sps == 0. Since sps = 4, 8 or 40 and
// 16380 are multiples of four, (n % 16380)*4 bytes is 16-byte aligned.
// At 8 sps 16380 % 8 == 4, so a bank wrap can split a symbol in half. The
// QPSK block wrapper uses aligned four-symbol boundaries (12/16 samples).
static IRAM_ATTR esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    const uint32_t before = esp_cpu_get_cycle_count();
    const stream_mark_t mark = dvb.profile ? stream_mark_for(&dvb.copy) : (stream_mark_t){0};
    const size_t requested = count;
    const size_t cache_words = dvb.cached_words;
    // radio_s3 calls this once to the physical bank end, then for the wrapped
    // prefix. Save only the suffix that survives that second callback.
    // radio_s3_bank() is 0x3fcc0000 or 0x3fcd0000: two 64 KiB aligned banks.
    // Use the physical pointer. Station ID removes samples from `first`, so
    // its modulo is neither the bank offset nor a reliable completion test.
    const size_t offset = ((uintptr_t)dst & 0xffff)/sizeof *dst;
    const bool bank_done = !offset || requested < RADIO_S3_BANK_WORDS-offset;
    const size_t save_words = offset && requested == RADIO_S3_BANK_WORDS - offset ?
        (offset < cache_words ? cache_words - offset : 0) : cache_words;
    // Copy any re-read samples before touching the ring. Fresh reads then move strictly
    // forwards, so a short second callback at the physical output wrap cannot shift hundreds
    // of cached symbols. Never read the other physical DAC bank: it may belong to the radio.
    if (enc.packed && first < dvb.sample_end) {
        const uint64_t back = dvb.sample_end - first;
        if (back > cache_words) return ESP_FAIL;
        size_t at = dvb.sample_at + cache_words - (size_t)back;
        if (at >= cache_words) at -= cache_words;
        const size_t cached = back < count ? (size_t)back : count;
        const size_t head = cached < cache_words - at ? cached : cache_words - at;
        shaper_copy_words(dst, dvb.samples + at, head);
        shaper_copy_words(dst + head, dvb.samples, cached - head);
        dst += cached, first += cached, count -= cached;
    }
    const unsigned sps = dvb.table.n;
    const uint32_t freed = dvb.ring.freed;
    // Packed reads unpack whole vectors when every symbol lands at an address of its own
    // parity (ring.h); the history is even, so shift the window by the first symbol's parity.
    // sps is 4 or 8 there, so that parity is bit log2(sps) of first. Legacy n40 keeps the window.
    uint8_t *win = dvb.win + (!enc.quads && enc.packed && !dvb.moon && (first & sps));
    uint64_t read_first = first*dvb.den, read_count = count*dvb.den;
    if (!dvb.moon && count && ring_window(&dvb.ring, win, dvb.win_bytes - 1, read_first, read_count, sps,
                    dvb.history, RADIO_S3_BANK_WORDS*dvb.den) < 0) return ESP_FAIL;
    const uint32_t copied = esp_cpu_get_cycle_count() - before;
    const uint64_t w0 = dvb.whole.cycles, p0 = dvb.partial.cycles;
    if (dvb.profile) stream_record(&dvb.copy, mark, stream_mark_for(&dvb.copy));
    uint32_t notified = 0, saved = 0;
    if (dvb.moon) {
        const stream_mark_t whole = dvb.profile ? stream_mark_for(&dvb.whole) : (stream_mark_t){0};
        if (count && moon_shape(dst,first,count) != ESP_OK) return ESP_FAIL;
        if (dvb.profile) stream_record(&dvb.whole, whole, stream_mark_for(&dvb.whole));
    } else if (enc.quads) {
        const stream_mark_t whole = dvb.profile ? stream_mark_for(&dvb.whole) : (stream_mark_t){0};
        shaper_qpsk_samples(dst, win + 2, first % (4*sps), count, dvb.table.blocks, sps);
        if (dvb.profile) stream_record(&dvb.whole, whole, stream_mark_for(&dvb.whole));
    } else (enc.packed ? shaper_samples_codes_profile : shaper_samples_profile)(dst,
        win + SHAPER_HISTORY, first % sps, count, &dvb.table,
        dvb.profile ? &dvb.whole : NULL, dvb.profile ? &dvb.partial : NULL);
    if (enc.packed && count) {
        // The completed bank retains exactly cache_words fresh samples in total.
        // Between callbacks the cache may be incomplete; the second is contiguous
        // and cannot re-read any sample after a first callback that shaped words.
        const stream_mark_t save = dvb.profile ? stream_mark_for(&dvb.save) : (stream_mark_t){0};
        const size_t keep = count < save_words ? count : save_words;
        const uint32_t *suffix = dst + count - keep;
        if (keep == cache_words) {
            shaper_copy_words(dvb.samples, suffix, keep);
            dvb.sample_at = 0;
        } else {
            const size_t head = keep < cache_words - dvb.sample_at ? keep :
                                cache_words - dvb.sample_at;
            shaper_copy_words(dvb.samples + dvb.sample_at, suffix, head);
            shaper_copy_words(dvb.samples, suffix + head, keep - head);
            dvb.sample_at += keep;
            if (dvb.sample_at >= cache_words) dvb.sample_at -= cache_words;
        }
        dvb.sample_end = first + count;
        if (dvb.profile) {
            const stream_mark_t after = stream_mark_for(&dvb.save);
            stream_record(&dvb.save, save, after);
            saved = after.cycles - save.cycles;
        }
    }
    // The wrapped prefix still needs the shaper SRAM. Hold a released slot's
    // notification until both callbacks finish; preserve it across callbacks.
    if (dvb.ring.freed != freed && enc.packed && !dvb.moon) dvb.wake_pending = true;
    if (dvb.wake_pending && bank_done) {
        const stream_mark_t wake = dvb.profile ? stream_mark_for(&dvb.notify) : (stream_mark_t){0};
        if (dvb.moon) moon_wake();
        else xTaskNotifyGive(enc.task);
        dvb.wake_pending = false;
        if (dvb.profile) {
            const stream_mark_t after = stream_mark_for(&dvb.notify);
            stream_record(&dvb.notify, wake, after);
            notified = after.cycles - wake.cycles;
        }
    }
    if (dvb.moon && bank_done) {
        if (dvb.wake_bank > dvb.wake_max) dvb.wake_max = dvb.wake_bank;
        dvb.wake_bank = 0;
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - before;
    dvb.fill_words += requested;
    dvb.shaped_words += count;
    dvb.reused_words += requested - count;
    dvb.fill_cycles += cycles;
    dvb.fill_calls++;
    if (cycles > dvb.max_fill_cycles) {
        dvb.max_fill_cycles = cycles;
        dvb.worst_copy = copied;
        dvb.worst_whole = dvb.whole.cycles - w0;
        dvb.worst_partial = dvb.partial.cycles - p0;
        dvb.worst_save = saved;
        dvb.worst_notify = notified;
    }
    return ESP_OK;
}

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

static void encoder_task(void *arg)
{
    encoder_t *e = arg;
    const size_t bytes = (e->mode->kbch - 80) / 8;
    uint8_t *const payload = e->payload;
    uint32_t x = SEED;
    const unsigned input = spi_input_type();
    dvbs2_ts_state ts = {0};
    if (spi_input_start() != ESP_OK) { e->failed = true; goto done; }
    e->input_started = true;
    if (dvb.moon && !input) {
        // Keep cold instruction/data misses outside playback, without another
        // frame buffer. Reset seed and all framing history before publication.
        const uint32_t before = esp_cpu_get_cycle_count();
        for (unsigned n = 0; n < 3; ++n) {
            uint32_t seed = SEED;
            synthetic_payload(e,bytes,&seed);
            if (dvbs2_fast_bb_gcs(e->fast,e->ws,payload,bytes*8)) { e->failed = true; goto done; }
            dvbs2_fast_bch(e->fast,e->ws);
            dvbs2_fast_ldpc(e->fast,e->ws);
            if (moon_map(true)) { e->failed = true; goto done; }
            ++e->warmup_frames;
        }
        e->warmup_cycles = esp_cpu_get_cycle_count()-before;
        dvb.mapper = (dvbs2_sign_state){0};
        dvb.last_word = dvb.page_blocks = 0;
    }
    for (uint32_t f = 0; !e->stop; ++f) {
        __atomic_store_n(&dvb.producer_phase,QUEUE_FEC,__ATOMIC_RELEASE);
        uint8_t *out = NULL;
        while (!dvb.moon && !(out = ring_slot(&dvb.ring, f))) {
            if (e->stop) {
                goto done;
            }
            e->slot_waits++;
            ulTaskNotifyTake(pdTRUE, e->packed ? portMAX_DELAY : 1);
            if (e->failed) goto done;
        }
        encoder_mark_t before = encoder_mark();
        const uint32_t start = before.clock.cycles;
        const encoder_mark_t frame_start = before;
        uint32_t input_cycles = 0;
        if (input) {
            size_t used = 0;
            while (used < bytes && !e->stop) {
                const uint32_t t0 = esp_cpu_get_cycle_count();
                const size_t got = spi_input_read(payload + used, bytes - used);
                input_cycles += esp_cpu_get_cycle_count() - t0;
                used += got;
                e->input_pending = used;
                if (used < bytes) {
                    if (got) e->input_partial++;
                    else {
                        e->input_waits++;
                        ulTaskNotifyTake(pdTRUE, 1);
                    }
                }
            }
            if (e->stop) goto done;
            if (input_cycles > e->input_frame_cycles_max) e->input_frame_cycles_max = input_cycles;
        } else synthetic_payload(e,bytes,&x);
        if (dvb.profile) {
            const encoder_mark_t after = encoder_mark();
            encoder_mark_t active = before;
            if (input) active.clock.cycles = after.clock.cycles-input_cycles;
            stream_record(&e->stage[0],active.clock,after.clock);
            // RX sleeps are excluded from input_cycles.
            before = after;
        }
        const encoder_mark_t kernel_start = dvb.profile ? before : encoder_mark();
        int rc;
        if (dvb.profile || dvb.moon) {
            rc = input == PV_TS ? dvbs2_fast_bb_ts(e->fast, e->ws, &ts, payload, bytes) :
                                  dvbs2_fast_bb_gcs(e->fast, e->ws, payload, bytes * 8);
            before = encoder_record(&e->stage[1],before);
            if (!rc) {
                dvbs2_fast_bch(e->fast, e->ws);
                before = encoder_record(&e->stage[2],before);
                dvbs2_fast_ldpc(e->fast, e->ws);
                before = encoder_record(&e->stage[3],before);
                if (dvb.moon) {
                    __atomic_store_n(&dvb.producer_phase,QUEUE_MAP,__ATOMIC_RELEASE);
                    rc = moon_map(false);
                } else if (e->packed) {
                    dvbs2_fast_map_packed(e->fast, e->ws, out);
                } else {
                    dvbs2_fast_map(e->fast, e->ws, out);
                }
                before = encoder_record(&e->stage[4],before);
                __atomic_store_n(&dvb.producer_phase,QUEUE_PL,__ATOMIC_RELEASE);
                if (!dvb.moon) {
                    if (e->packed) dvbs2_fast_pl_packed(e->fast,out);
                    else dvbs2_fast_pl(e->fast,out);
                }
                before = encoder_record(&e->stage[5],before);
            }
        } else if (e->packed) {
            rc = dvbs2_encode_frame_packed(e->fast, e->ws, input == PV_TS ? &ts : NULL, payload, bytes * 8, out);
        } else {
            rc = dvbs2_encode_frame(e->fast, e->ws, input == PV_TS ? &ts : NULL, payload, bytes * 8, out);
        }
        if (rc) {
            if (e->stop) goto done;
            e->failed = true;
            goto done;
        }
        const encoder_mark_t end = encoder_mark();
        // As with waiting for a legacy frame slot, full-queue backpressure is
        // outside encoder work. Interrupts and runnable-task delays stay timed.
        const uint32_t kernel_spent = end.clock.cycles-kernel_start.clock.cycles-
                                      (uint32_t)(end.wait-kernel_start.wait);
        const uint32_t spent = input ? input_cycles+kernel_spent :
            end.clock.cycles-start-(uint32_t)(end.wait-frame_start.wait);
        const uint32_t work = kernel_spent;
        const uint32_t frame_work = spent;
        e->frame_work_cycles += frame_work;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && frame_work > e->frame_work_live_max) e->frame_work_live_max = frame_work;
        e->work_cycles += work;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && work > e->work_live_max) e->work_live_max = work;
        e->input_pending = 0;
        if (!dvb.moon) ring_publish(&dvb.ring, f);
        e->frames = f + 1;
        e->cycles += spent;
        e->kernel_cycles += kernel_spent;
        e->min_cycles = spent < e->min_cycles ? spent : e->min_cycles;
        e->max_cycles = spent > e->max_cycles ? spent : e->max_cycles;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && spent > e->live_max_cycles) e->live_max_cycles = spent;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && kernel_spent > e->kernel_live_max) e->kernel_live_max = kernel_spent;
        if (e->stall_ms) {
            vTaskDelay(pdMS_TO_TICKS(e->stall_ms));
        }
    }
done:
    spi_input_stop();
    e->stack_min_free = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(e->exited);
    // Keep the notification handle valid even if encoding failed. The command owns
    // deletion after taking exited; a fill may still notify this suspended task.
    vTaskSuspend(NULL);
}

static void release(void)
{
    spi_input_buffers(NULL, 0, NULL, 0);
    if (cache_configured) {
        const uint32_t regs[] = {EXTMEM_IBUS_TO_FLASH_START_VADDR_REG,
            EXTMEM_IBUS_TO_FLASH_END_VADDR_REG, EXTMEM_DBUS_TO_FLASH_START_VADDR_REG,
            EXTMEM_DBUS_TO_FLASH_END_VADDR_REG};
        for (unsigned i = 0; i < 4; ++i) REG_WRITE(regs[i], cache_ranges[i]);
        cache_configured = false;
    }
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_free(dvb.samples);
    dvb.samples = NULL;
    heap_caps_free(enc.tables);
    heap_caps_free(enc.pair_bch);
    heap_caps_free(enc.fast);
    if (!enc.input_borrowed) heap_caps_free(enc.input);
    enc.input = NULL;
    if (!enc.ws_borrowed) heap_caps_free(enc.ws);
    if (dvb.win && !dvb.moon && (!enc.packed || (enc.mode->bits_per_symbol == 3 && dvb.table.n != 4)))
        heap_caps_free(dvb.win);
    dvb.win = NULL;
    if (!enc.bch_borrowed) heap_caps_free(enc.bch);
    heap_caps_free(enc.block);
    if (!enc.packed) {
        heap_caps_free(enc.payload);
        heap_caps_free(enc.slots);
        heap_caps_free(enc.filler);
    }
    enc.tables = enc.pair_bch = NULL;
    enc.fast = NULL, enc.ws = NULL, enc.bch = enc.block = enc.payload = enc.slots = enc.filler = NULL;
}

// Bytes for a packed part, 16-byte aligned, with the 16 spare bytes the vector unpack may read.
static size_t packed_bytes(size_t symbols)
{
    return (symbols / 2 + 16 + 15) & ~(size_t)15;
}

// Build the scaled table and the dummy frame, start the encoder on core 0 and wait until it has
// filled every slot. Returns the frame length in symbols, or 0 after printing an error for `cmd`.
static size_t start_encoder(const char *cmd, unsigned modcod, unsigned level, unsigned stall_ms,
                            unsigned sps, uint32_t dac_rate)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
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
    const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
    dvb.dac_rate = dac_rate;
    dvb.moon = moon;
    dvb.lofty = lofty;
    dvb.page_wake = !lofty;
    dvb.queue_pages = lofty ? (modcod == 17 ? 46 : modcod == 13 ? 54 : LOFTY_PAGES) : MACHINE_PAGES;
    dvb.wake_pages = lofty ? LOFTY_WAKE : MACHINE_WAKE_PAGES;
    dvb.pages = NULL;
    dvb.lofty_pages = NULL;
    dvb.last_word = 0;
    dvb.den = fractional ? 5 : 1;
    dvb.span = lofty ? 5 : moon ? 7 : 9;
    dvb.history = moon ? (fractional ? 10 : 8) : SHAPER_HISTORY;
    dvb.cached_words = CACHED_FOR(dac_rate);
    (void)heap_caps_monitor_local_minimum_free_size_start();   // low-water mark of this command
    memset(&enc, 0, sizeof enc);
    enc.mode = mode;
    memset(&dvb.copy, 0, sizeof dvb.copy);
    memset(&dvb.whole, 0, sizeof dvb.whole);
    memset(&dvb.partial, 0, sizeof dvb.partial);
    memset(&dvb.save, 0, sizeof dvb.save);
    memset(&dvb.notify, 0, sizeof dvb.notify);
    dvb.fill_words = dvb.fill_cycles = dvb.fill_calls = 0;
    dvb.worst_copy = dvb.worst_whole = dvb.worst_partial = 0;
    dvb.worst_save = dvb.worst_notify = 0;
    dvb.shaped_words = dvb.reused_words = 0;
    dvb.sample_end = dvb.sample_at = 0;
    dvb.wake_pending = false;
    dvb.profile = !strcmp(cmd, "dvbpace") || !strcmp(cmd, "dvbdump");
    stream_jitter_enable(0);
    // The fill phases of a timed run record cycles only; an untimed dump also reads the
    // shared cache counters. Encoder stages always read them.
    dvb.copy.counters = dvb.whole.counters = dvb.partial.counters = !strcmp(cmd, "dvbdump");
    dvb.save.counters = dvb.notify.counters = dvb.copy.counters;
    if (dvb.profile) cache_profile_start();
    enc.stall_ms = stall_ms;
    enc.min_cycles = UINT32_MAX;
    for (unsigned i = 0; i < 6; ++i) {
        enc.stage[i].counters = 1;
    }
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t payload_bytes = (mode->kbch - 80) / 8;
    const bool input = spi_input_type() != 0;
    size_t store_used = shaper_table_words(sps, mode->bits_per_symbol) * 4;
    enc.packed = sps != 40;
    enc.quads = quads;
    dvb.table.n = sps;
    // QPSK n3 uses two slots and an 8144-byte fast workspace in the store.
    // At 10 Msym/s the 5/6 encoder needs about 69% of a frame period; a two-frame model keeps
    // as little as 244 symbols of lead there, three frames keep 18,871 even at 95% (test_ring).
    const unsigned nslots = moon ? 0 : !enc.packed ? RING_SLOTS : (quads && sps == 3) ? 2 : sps <= 4 ? 3 : 2;
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
        dvb.prepare_cycles = 0;
        dvb.prepare_pages = 0;
        if (lofty) lofty_build(shaper_store,level);
        else machine_build(shaper_store,level);
        dvb.page_blocks = 0;
        dvb.mapper = (dvbs2_sign_state){0};
        dvb.queue_wait_cycles = dvb.queue_wait_max = dvb.queue_wait_live_max = dvb.prepare_ticks = 0;
        dvb.page_clock = dvb.page_gap_max = dvb.page_wait = 0;
        uint8_t *tail;
        if (lofty) {
            dvb.lofty_pages = (lofty_page *)(shaper_store+LOFTY_TABLE_WORDS);
            tail = (uint8_t *)(dvb.lofty_pages+LOFTY_STORED);
            if (dvb.queue_pages > LOFTY_STORED) {
                enc.block = heap_caps_aligned_alloc(16,(dvb.queue_pages-LOFTY_STORED)*sizeof(lofty_page),internal);
                if (!enc.block) { release(); return 0; }
            }
            moon_ws = (dvbs2_workspace *)tail;
            tail += DVBS2_FAST_WORKSPACE_USED;
        } else {
            dvb.pages = (machine_page *)(shaper_store+MACHINE_TABLE_WORDS);
            uint32_t *lut = (uint32_t *)(dvb.pages+MACHINE_PAGES);
            machine_lut(lut);
            dvb.sign_lut = lut;
            moon_ws = (dvbs2_workspace *)(lut+MACHINE_LUT_WORDS);
            tail = (uint8_t *)moon_ws+DVBS2_FAST_WORKSPACE_USED;
        }
        dvb.win = NULL;
        dvb.win_bytes = 0;
        store_used = tail-(uint8_t *)shaper_store;
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
        const size_t frame = packed_bytes(symbols), dummy = packed_bytes(DVBS2_DUMMY_SYMBOLS);
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
    // Keep its context there too when SPI is selected, reserving internal RAM for Wi-Fi.
    enc.fast = heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode),
                                      input && enc.packed && !resident ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : internal);
    enc.ws_borrowed = quads || moon;
    enc.ws = moon ? moon_ws : enc.ws_borrowed && sps == 4 ? (dvbs2_workspace *)(shaper_store + QPSK_TABLE_WORDS(4)) :
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
        const size_t tail = dvbs2_fast_cache_tail_size(enc.fast);
        uint8_t *shared = store_used+tail <= sizeof shaper_store ? (uint8_t *)shaper_store+store_used : NULL;
        const size_t heap = cache-(shared ? tail : 0);
        if (shared) store_used += tail;
        enc.tables = heap_caps_aligned_alloc(16,heap+(lofty ? DVBS2_LOFTY_CACHE_BYTES : 0),internal);
        if (!enc.tables || dvbs2_fast_cache_split(enc.fast,enc.tables,shared) ||
            (lofty && dvbs2_fast_lofty_cache(enc.fast,(uint32_t *)(enc.tables+heap)))) {
            heap_caps_free(taps); release();
            printf("@{\"cmd\":\"%s\",\"error\":\"encoder cache allocation failed\"}\n",cmd);
            return 0;
        }
    }
    if (lofty && mode->t > 8) {
        enc.pair_bch = heap_caps_aligned_alloc(16,DVBS2_BCH_PAIR_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!enc.pair_bch || dvbs2_fast_cache_bch_pair(enc.fast,enc.pair_bch)) {
            printf("@{\"cmd\":\"%s\",\"error\":\"BCH pair allocation failed\"}\n",cmd);
            heap_caps_free(taps); release(); return 0;
        }
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

    if (moon && !input && !strcmp(cmd,"dvbpace")) {
        const size_t at = (store_used+15) & ~(size_t)15;
        enc.input_borrowed = at+payload_bytes <= sizeof shaper_store;
        enc.input = enc.input_borrowed ? (uint8_t *)shaper_store+at : heap_caps_malloc(payload_bytes,internal);
        if (!enc.input) {
            release();
            printf("@{\"cmd\":\"%s\",\"error\":\"internal input allocation failed\"}\n",cmd);
            return 0;
        }
        uint32_t x = SEED;
        const uint32_t before = esp_cpu_get_cycle_count();
        payload_fill(enc.input,payload_bytes,&x);
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

    // Bounded bursts above Wi-Fi, then sleep on the full queue. Startup and
    // stop need no controller boost because a full producer always blocks.
    enc.priority = moon ? MOON_PRIORITY : ENCODER_PRIORITY;
    enc.exited = xSemaphoreCreateBinaryStatic(&exited_storage);
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", ENCODER_STACK, &enc, enc.priority, &enc.task, 0)
            != pdPASS) {
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder task failed\"}\n", cmd);
        release();
        return 0;
    }
    const bool external = spi_input_type() != 0;
    const TickType_t limit = xTaskGetTickCount() + pdMS_TO_TICKS(external ? 100 : (moon ? 3 : nslots) * (stall_ms + 100));
    while ((moon ? __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed < dvb.queue_pages :
            __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE) < nslots) && !enc.failed &&
           (int32_t)(xTaskGetTickCount()-limit) < 0) vTaskDelay(1);
    if (enc.failed || (external ? !enc.input_started : moon ? dvb.produced-dvb.consumed < dvb.queue_pages :
            __atomic_load_n(&dvb.ring.published,__ATOMIC_ACQUIRE) < nslots)) {
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

// Stop the encoder after a run and print the stream part of the result record.
static void print_profile(const char *name, const stream_profile_t *p)
{
    printf("\"%s\":{\"calls\":%" PRIu32 ",\"cycles\":%" PRIu64 ",\"max_cycles\":%" PRIu32
           ",\"flash_miss\":%" PRIu64 ",\"psram_miss\":%" PRIu64
           ",\"data_access\":%" PRIu64 ",\"instruction_miss\":%" PRIu64 ",\"prepare_cycles\":%" PRIu64 "}",
           name, p->calls, p->cycles, p->max_cycles, p->flash_miss, p->psram_miss,
           p->data_access, p->instruction_miss, p->prepare_cycles);
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
    ring_t *r = &dvb.ring;
    if (dvb.moon) {
        r->next_frame = dvb.queue_sample*(dvb.lofty ? 4 : 5)/(12*r->frame_symbols);
        r->next_position = r->next_frame;
    }
    spi_input_print();
    printf(",\"fast_memory\":\"%s\"", spi_input_type() && enc.mode->bits_per_symbol == 3 && dvb.table.n == 8 ? "psram" : "internal");
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
           dvb.lofty ? "lofty-queue" : dvb.moon ? "moon-queue" : "pie");
    printf(",\"payload_source\":\"%s\",\"input_frames\":%u,\"input_memory\":\"%s\",\"input_prepare_cycles\":%" PRIu32
           ",\"encoder_priority\":%u,\"encoder_pie\":%u,\"encode_frame_work_cycles_mean\":%" PRIu64 ",\"encode_frame_work_cycles_live_max\":%" PRIu32,
           enc.input ? "internal-frame" : spi_input_type() ? "spi" : "xorshift32",enc.input ? 1 : 0,
           enc.input ? "internal" : "none",enc.input_prepare_cycles,enc.priority,dvbs2_fast_pie(enc.fast),
           enc.frames ? enc.frame_work_cycles/enc.frames : 0,enc.frame_work_live_max);
    if (dvb.moon) printf(",\"queue_wait_cycles\":%" PRIu64 ",\"queue_wait_max_cycles\":%u,\"queue_wait_live_max_cycles\":%u,\"prepare_max_ticks\":%u,\"queue_page_gap_max_cycles\":%u,\"queue_page_samples\":%u,\"queue_wake_pages\":%u",
                         dvb.queue_wait_cycles,(unsigned)dvb.queue_wait_max,(unsigned)dvb.queue_wait_live_max,(unsigned)dvb.prepare_ticks,(unsigned)dvb.page_gap_max,MACHINE_PAGE_SAMPLES,dvb.wake_pages);
    if (dvb.moon) printf(",\"queue_wake_policy\":\"%s\"",dvb.page_wake ? "page" : "bank");
    if (dvb.moon && dvb.queue_empty) printf(",\"queue_fault\":{\"produced\":%"PRIu32",\"consumed\":%"PRIu32
        ",\"producer_phase\":%"PRIu32",\"last_wait_produced\":%"PRIu32",\"last_wait_consumed\":%"PRIu32"}",
        dvb.queue_fault.put,dvb.queue_fault.get,dvb.queue_fault.phase,
        dvb.queue_fault.wait_put,dvb.queue_fault.wait_get);
    if (dvb.moon) printf(",\"bch_pair_psram_bytes\":%u",enc.pair_bch ? DVBS2_BCH_PAIR_BYTES : 0);
    if (dvb.moon) printf(",\"warmup_frames\":%u,\"warmup_cycles\":%u",(unsigned)enc.warmup_frames,(unsigned)enc.warmup_cycles);
    if (dvb.moon) printf(",\"encode_work_cycles_mean\":%" PRIu64 ",\"encode_work_cycles_live_max\":%" PRIu32,
                         enc.frames ? enc.work_cycles/enc.frames : 0,enc.work_live_max);
    if (dvb.moon) printf(",\"queue_pages\":%u,\"queue_low_pages\":%u,\"queue_empty\":%u,"
                         "\"queue_wake_max_cycles\":%u,\"queue_wakes\":%u,\"prepare_pages\":%u,"
                         "\"prepare_max_cycles\":%u,\"prepare_tail_max_cycles\":%u,\"prepare_cycles\":%" PRIu64 ","
                         "\"table_bytes\":%u,\"table_heap_bytes\":0,\"encoder_cache_bytes\":%u",
                         dvb.queue_pages,(unsigned)dvb.queue_low,(unsigned)dvb.queue_empty,(unsigned)dvb.wake_max,(unsigned)dvb.queue_wakes,
                         (unsigned)dvb.prepare_pages,(unsigned)dvb.prepare_max,(unsigned)dvb.prepare_tail_max,dvb.prepare_cycles,(dvb.lofty ? LOFTY_TABLE_WORDS : MACHINE_TABLE_WORDS)*4,
                         (unsigned)(dvbs2_fast_cache_size(enc.fast)+(dvb.lofty ? DVBS2_LOFTY_CACHE_BYTES : 0)));
    if (dvb.moon) {
        const size_t total = dvbs2_fast_cache_size(enc.fast)+(dvb.lofty ? DVBS2_LOFTY_CACHE_BYTES : 0);
        const size_t heap = heap_caps_get_allocated_size(enc.tables);
        printf(",\"encoder_cache_heap_bytes\":%u,\"encoder_cache_store_bytes\":%u",
               (unsigned)heap,(unsigned)(total-heap));
    }
    printf(",\"workspace_memory\":\"%s\",\"workspace_bytes\":%u,\"fast_bytes\":%u,"
           "\"workspace_heap_bytes\":%u,\"fast_heap_bytes\":%u,\"bch_cache_heap_bytes\":%u,"
           "\"bch_cache_bytes\":%u,\"bch_variant\":\"%s\",\"packed_mapper\":\"%s\"",
           enc.packed && (dvb.moon || enc.mode->bits_per_symbol == 2 || dvb.table.n == 4) ? "internal" : "psram", (unsigned)sizeof *enc.ws,
           (unsigned)dvbs2_fast_mode_size(enc.mode),
           enc.ws_borrowed ? 0 : (unsigned)heap_caps_get_allocated_size(enc.ws),
           (unsigned)heap_caps_get_allocated_size(enc.fast),
           enc.bch && !enc.bch_borrowed ? (unsigned)heap_caps_get_allocated_size(enc.bch) : 0,
           enc.bch || enc.tables ? DVBS2_BCH_CACHE_BYTES : 0,
           enc.tables ? (enc.pair_bch ? "split-pair" : enc.mode->t == 8 ? "resident-pair" : "resident-byte") : enc.bch ? "resident-byte" : enc.packed && (dvb.moon || enc.mode->bits_per_symbol == 2 || dvb.table.n == 4)
                                      && enc.mode->t == 8 && (enc.quads || enc.mode->bits_per_symbol == 3) && !spi_input_type() ? "flash-pair" : "nibble",
           !enc.packed ? "none" : dvb.lofty ? "words" : dvb.moon ? "signs" : enc.quads ? "quads" : enc.mode->bits_per_symbol == 2 || dvb.table.n == 4 ? "direct" : "sparse");
    printf(",\"encoder_stack_bytes\":%u,\"encoder_stack_min_free\":%u", ENCODER_STACK, (unsigned)enc.stack_min_free);
    printf(",\"ring_memory\":\"%s\",\"ring_format\":\"%s\",\"ring_heap_bytes\":%u,"
           "\"payload_memory\":\"%s\",\"slot_waits\":%" PRIu32,
           enc.packed ? "internal" : "psram", dvb.lofty ? "words" : dvb.moon ? "signs" : enc.quads ? "quads" : enc.packed ? "packed" : "raw",
           enc.block ? (unsigned)heap_caps_get_allocated_size(enc.block) : 0,
           enc.packed ? "internal" : "psram", enc.slot_waits);
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
    printf("]}");
    if (dvb.profile) {
        printf(",\"profile\":{\"cache_scope\":\"both cores and interrupts; windows overlap\","
               "\"fill_counters\":%s,\"fill_calls\":%" PRIu32 ",\"fill_words\":%" PRIu64
               ",\"fill_cycles\":%" PRIu64 ",\"worst_call\":{\"copy\":%" PRIu32 ",\"whole\":%" PRIu32
               ",\"partial\":%" PRIu32 ",\"save\":%" PRIu32 ",\"notify\":%" PRIu32 "},"
               "\"shaped_words\":%" PRIu64 ",\"reused_words\":%" PRIu64 ",\"fill\":{",
               dvb.copy.counters ? "true" : "false", dvb.fill_calls, dvb.fill_words,
               dvb.fill_cycles, dvb.worst_copy, dvb.worst_whole, dvb.worst_partial,
               dvb.worst_save, dvb.worst_notify, dvb.shaped_words, dvb.reused_words);
        print_profile("copy", &dvb.copy); printf(",");
        print_profile("whole", &dvb.whole); printf(",");
        print_profile("partial", &dvb.partial); printf(",");
        print_profile("save", &dvb.save); printf(",");
        print_profile("notify", &dvb.notify); printf("},\"encoder\":{");
        static const char *const names[] = {"payload", "bb", "bch", "ldpc", "map", "pl"};
        for (unsigned i = 0; i < 6; ++i) {
            if (i) printf(",");
            print_profile(names[i], &enc.stage[i]);
        }
        printf("}}");
    }
    stream_jitter_print();
    release();
}

static void print_mode(const char *cmd, unsigned modcod, size_t symbols, unsigned level)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    printf("@{\"cmd\":\"%s\",\"modcod\":%u,\"rate\":\"%s\",\"symbol_rate\":%u,\"rolloff\":%.2f,"
           "\"pilots\":%d,\"frame_symbols\":%u,\"payload_bytes_per_frame\":%u,\"peak_level\":%u",
           cmd, modcod, mode->rate, (unsigned)((uint64_t)dvb.dac_rate*dvb.den / dvb.table.n), dvb.moon && dvb.den == 5 ? .20 : .35, PILOTS, (unsigned)symbols,
           (unsigned)((mode->kbch - 80) / 8), level);
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

static void print_wifi(const radio_s3_wifi_heap_t *w)
{
    printf(",\"wifi_free_before\":%" PRIu32 ",\"wifi_largest_before\":%" PRIu32
           ",\"wifi_free_init\":%" PRIu32 ",\"wifi_largest_init\":%" PRIu32
           ",\"wifi_free_up\":%" PRIu32 ",\"wifi_largest_up\":%" PRIu32
           ",\"wifi_min_free\":%" PRIu32, w->free_before, w->largest_before, w->free_init,
           w->largest_init, w->free_up, w->largest_up, w->min_free);
}

void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps, uint32_t dac_rate)
{
    if (!station_require("dvb")) {
        return;
    }
    const size_t symbols = start_encoder("dvb", modcod, level, 0, sps, dac_rate);
    if (!symbols) {
        return;
    }
    radio_s3_stats_t s = {0};
    // RRC support B = Rs * (1 + alpha), alpha = 0.20 or 0.35. Round upward in integer Hz.
    const uint32_t bandwidth = ((uint64_t)dac_rate * dvb.den * (dvb.den == 5 ? 120 : 135) + sps * 100 - 1) / (sps * 100);
    const esp_err_t err = radio_s3_run_bandwidth(freq, (uint8_t)gain, seconds, dac_rate, bandwidth,
                                               fill, NULL, &s);
    print_mode("dvb", modcod, symbols, level);
    printf(",\"freq_mhz\":%" PRIu32 ",\"seconds\":%" PRIu32 ",\"gain\":%u,\"dac_rate\":%" PRIu32
           ",\"step_words\":%u", freq, seconds, gain, dac_rate, RADIO_S3_STEP_FOR(dac_rate));
    printf(",\"pulse_symbols\":%u", dvb.moon ? dvb.span : dvb.table.parts == 2 ? 6 : 9);
    print_timing(&s);
    printf(",\"fe_gain_index\":%" PRIu32 ",\"chain\":\"cwgain\"", s.fe_gain_index);
    radio_filter_print(&s.filter);
    print_wifi(&s.wifi);
    stop_encoder();
    printf(",\"status\":\"%s\"%s}\n", esp_err_to_name(err),
           err == ESP_ERR_INVALID_ARG ? ",\"error\":\"duration too short for start/end ID\"" :
           err != ESP_OK ? ",\"error\":\"radio run failed\"" : "");
}

void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps, uint32_t dac_rate, unsigned wake_policy, bool observer)
{
    const size_t symbols = start_encoder("dvbpace", modcod, level, stall_ms, sps, dac_rate);
    if (!symbols) {
        return;
    }
    if (wake_policy) {
        dvb.page_wake = wake_policy == 1;
        if (dvb.lofty) dvb.wake_pages = LOFTY_WAKE;
    }
    stream_jitter_enable(observer && dvb.moon);
    radio_s3_stats_t s;
    const esp_err_t err = (dvb.moon ? radio_s3_paced_wifi_rate : radio_s3_paced_run_rate)(seconds, dac_rate, fill, NULL, &s);
    print_mode("dvbpace", modcod, symbols, level);
    printf(",\"paced_wifi_up\":%s,\"jitter_enabled\":%s",dvb.moon ? "true" : "false",observer && dvb.moon ? "true" : "false");
    if (dvb.moon) print_wifi(&s.wifi);
    printf(",\"seconds\":%" PRIu32 ",\"dac_rate\":%" PRIu32 ",\"step_words\":%u", seconds,
           dac_rate, RADIO_S3_STEP_FOR(dac_rate));
    printf(",\"pulse_symbols\":%u", dvb.moon ? dvb.span : dvb.table.parts == 2 ? 6 : 9);
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
                unsigned stall_ms, unsigned sps, uint32_t dac_rate)
{
    const size_t symbols = start_encoder("dvbdump", modcod, level, stall_ms, sps, dac_rate);
    if (!symbols) {
        return;
    }
    const uint64_t samples = ((uint64_t)frames * symbols * dvb.table.n+dvb.den-1)/dvb.den;
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
    const esp_err_t err = radio_s3_dry_run_rate(fill, NULL, samples, dac_rate, sink, d, &s);
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

void dvbtx_wifi(unsigned modcod, unsigned sps, uint32_t dac_rate)
{
    if (!station_require("wifimem")) {
        return;
    }
    size_t symbols = 0;
    if (modcod) {
        symbols = start_encoder("wifimem", modcod, 128, 0, sps, dac_rate);
        if (!symbols) {
            return;
        }
    } else {
        (void)heap_caps_monitor_local_minimum_free_size_start();
    }
    radio_s3_stats_t s;
    const esp_err_t err = radio_s3_wifi_probe(&s);
    if (modcod) {
        print_mode("wifimem", modcod, symbols, 128);
        printf(",\"dac_rate\":%" PRIu32 ",\"pulse_symbols\":%u", dac_rate,
               dvb.moon ? dvb.span : dvb.table.parts == 2 ? 6 : 9);
    } else {
        printf("@{\"cmd\":\"wifimem\",\"modcod\":0");
    }
    print_wifi(&s.wifi);
    printf(",\"id_samples\":%" PRIu32 ",\"id_count\":%" PRIu32, s.id_samples, s.id_count);
    if (modcod) {
        stop_encoder();
    } else {
        printf(",\"free_internal\":%u,\"largest_internal\":%u,\"min_free_internal\":%u",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        (void)heap_caps_monitor_local_minimum_free_size_stop();
    }
    printf(",\"status\":\"%s\"%s}\n", esp_err_to_name(err),
           err != ESP_OK ? ",\"error\":\"wifi probe failed\"" : "");
}
