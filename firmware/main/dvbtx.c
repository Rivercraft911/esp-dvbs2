// QPSK defaults to 1 Msym/s; both modulations accept 5m/10m at 40 MS/s and
// 20m at 80 MS/s. Fast rates use packed internal rings and cached samples.
// Paired QPSK n4/n8 tables share the store with ring and payload. The block
// kernels use four codes per byte; n3 shares with its ring, n4 with its workspace.
// Payload: xorshift32 seed 2026, one contiguous byte stream across data frames.
// On underrun the ring inserts a 3330-symbol dummy PLFRAME (EN 302 307-1 5.1.5,
// 5.5.1), preserving each choice for overlapping bank reads. No payload is skipped.
// Legacy RRC roll-off 0.35 and fractional 0.20 match the BBHEADER. Legacy scaling uses the
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
#include "station_id.h"
#include "station_store.h"
#include "station_commands.h"
#include "shaper.h"
#include "shaper_qpsk.h"
#include "shaper_taps.h"
#include "shaper_moon.h"

#define RING_SLOTS 8                // legacy QPSK frames ahead: 233 ms at 1 Msym/s
#define PACKED_SLOTS 3              // fast rates: three packed frames at 4 sps, two at 8, plus a cache
#define PILOTS 1
#define BANK_SYMBOLS 4096           // ceil(16380/4) + 1, worst offset at 4 sps
#define TAPS (9 * 40)
#define WORST_PEAK 718.4            // conservative sqrt(2)*508 LSB axis bound, before rounding
#define SEED 2026
#define ENCODER_PRIORITY 5          // above the command loop, below the Wi-Fi and IPC tasks
// Samples, including the first bank's extra lead. Later banks re-read 2112 words, or 640 on the
// 80 MS/s geometry, which keeps only its 768 (dvb.cached_words).
#define CACHED_FOR(rate) (RADIO_S3_BANK_WORDS - RADIO_S3_STEP_FOR(rate) + RADIO_S3_LEAD_FOR(rate))
#define CACHED_WORDS CACHED_FOR(RADIO_S3_DAC_RATE)
_Static_assert(CACHED_FOR(RADIO_S3_DAC_RATE_80M) <= CACHED_WORDS, "sample cache too small");
// Finished samples cover the overlap. The next fresh sample only needs filter history and
// one symbol cut at each end, even when the output wraps in the middle of an 8-sps symbol.
#define OVERLAP_SYMBOLS 16 // twelve history symbols plus cut edges in the rational path

typedef struct {
    ring_t ring;
    shaper_table_t table;
    uint8_t *win;                  // internal; borrows the tail of each internal fast workspace
    size_t win_bytes;
    uint8_t overlap[OVERLAP_SYMBOLS];
    uint32_t samples[CACHED_WORDS] __attribute__((aligned(16)));
    uint64_t sample_end;            // cached samples end here; only advances on fresh samples
    size_t sample_at;               // circular cache index of sample_end
    uint32_t max_fill_cycles;       // one fill call
    bool profile;                  // radio-off commands only
    unsigned den, history, span;   // exact sps = table.n / den
    bool moon;                     // block kernels split between both cores
    unsigned moon_mode, helper_num, helper_den;
    const uint32_t *moon_tables[5];
    uint32_t *moon_extra;
    uint16_t *blocks;              // workspace tail, frozen until helper completion
    TaskHandle_t helper;
    volatile bool helper_stop;
    unsigned pending;             // release/acquire single job, no queue
    uint32_t *job_dst;
    const uint8_t *job_src;
    unsigned job_offset;
    unsigned job_skip;
    size_t job_count;
    uint64_t helper_cycles;
    uint32_t helper_max, dispatch_max, join_max;
    uint32_t helper_bank, helper_bank_max;
    uint64_t helper_bank_end;
    uint32_t helper_jobs, encoder_freed;
    uint32_t dac_rate;             // the run's bank geometry
    size_t cached_words;           // active sample cache, within samples[]
    stream_profile_t copy, whole, partial, save, notify;
    uint64_t fill_words, fill_cycles;
    uint32_t fill_calls;
    uint32_t worst_copy, worst_whole, worst_partial, worst_save, worst_notify;
    uint64_t shaped_words, reused_words;
} dvb_t;

static DRAM_ATTR dvb_t dvb;
_Static_assert(sizeof(dvbs2_workspace) - DVBS2_FAST_WORKSPACE_USED >=
               SHAPER_HISTORY + BANK_SYMBOLS + 1, "fast workspace tail holds n4 window");
_Static_assert(sizeof(dvbs2_workspace)-DVBS2_FAST_WORKSPACE_USED >=
               2*((RADIO_S3_BANK_WORDS+11)/12+5), "workspace tail holds both moon chunks");

typedef struct {
    const dvbs2_mode *mode;
    dvbs2_fast *fast;               // fast encoder context for this mode, internal RAM
    dvbs2_workspace *ws;            // internal for fast QPSK / 4-sps 8PSK, otherwise PSRAM
    uint8_t *bch;                  // resident byte table with internal workspace, t = 10/12
    bool quads;                     // four QPSK codes/byte, block shaper
    bool packed;                    // all fast rates: packed internal ring
    uint8_t *payload;               // one frame of payload: next to a packed ring, or PSRAM
    uint8_t *slot[PACKED_SLOTS];    // packed frames, internal
    uint8_t *block;                 // packed ring's heap part: slot 2 (4 sps) or everything (8 sps)
    uint8_t *slots, *filler;        // QPSK: PSRAM; packed: filler is the packed dummy
    TaskHandle_t task;
    uint32_t stall_ms;
    volatile bool stop;
    bool failed;
    SemaphoreHandle_t exited;
    uint32_t frames;                // encoded and published
    uint64_t cycles;                // per frame: payload and encoding, raw
    uint32_t min_cycles, max_cycles, live_max_cycles; // live excludes the initial ring prefill
    uint64_t kernel_cycles;        // same-frame interval after payload generation
    uint32_t kernel_live_max;
    uint64_t work_cycles;          // kernel wall interval minus preempting helper
    uint32_t work_live_max;
    stream_profile_t stage[6];      // payload, BB, BCH, LDPC, map, PL
    uint32_t slot_waits;
} encoder_t;

static encoder_t enc;
static StaticSemaphore_t exited_storage, helper_exited_storage;
static SemaphoreHandle_t dvb_helper_exited;
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

// One higher-priority helper on core 0 preempts the encoder. Only the fill
// publishes a job. Its staged input and the disjoint destination stay owned
// until acquire observes completion. CCOUNT deltas never cross cores.
// The helper already runs on core 0, so slot release needs no second IPI.
// No job is outstanding when the fill uses the short local-only path.
static IRAM_ATTR void moon_wake_encoder(void)
{
    const uint32_t freed = __atomic_load_n(&dvb.ring.freed,__ATOMIC_ACQUIRE);
    if (freed != dvb.encoder_freed) {
        dvb.encoder_freed = freed;
        xTaskNotifyGive(enc.task);
    }
}

static void moon_helper(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (dvb.helper_stop) break;
        if (!__atomic_load_n(&dvb.pending, __ATOMIC_ACQUIRE)) continue;
        const uint32_t before = esp_cpu_get_cycle_count();
        shaper_moon_pack(dvb.blocks,dvb.job_src,dvb.job_offset,(dvb.job_skip+dvb.job_count+11)/12+2,dvb.moon_mode);
        shaper_moon_sample_tables(dvb.job_dst,dvb.blocks+2,dvb.job_skip,dvb.job_count,
                                  dvb.moon_tables,dvb.moon_mode);
        moon_wake_encoder();
        const uint32_t cycles = esp_cpu_get_cycle_count()-before;
        dvb.helper_cycles += cycles;
        if (cycles > dvb.helper_max) dvb.helper_max = cycles;
        dvb.helper_bank += cycles;
        ++dvb.helper_jobs;
        __atomic_store_n(&dvb.pending, 0, __ATOMIC_RELEASE);
    }
    // Same stop handshake as the encoder. The command owns deletion.
    xSemaphoreGive(dvb_helper_exited);
    vTaskSuspend(NULL);
}

static IRAM_ATTR void moon_shape(uint32_t *dst, uint64_t first, size_t count,
                                 const uint8_t *win, unsigned offset)
{
    if (!count) return;
    const unsigned skip = first%12;
    const unsigned groups = (skip+count+11)/12;
    // Small fixed fractions leave core 0 room for the serial encoder. Recompute
    // admission after kernel changes; this is not an adaptive scheduler.
    const size_t split = ((count*dvb.helper_num/dvb.helper_den+skip)/12)*12;
    const size_t helper = split > skip ? split-skip : 0;
    if (!helper) {
        moon_wake_encoder();
        shaper_moon_pack(dvb.blocks,win,offset,groups+2,dvb.moon_mode);
        shaper_moon_sample_tables(dst,dvb.blocks+2,skip,count,dvb.moon_tables,dvb.moon_mode);
        return;
    }
    dvb.job_dst = dst;
    dvb.job_src = win;
    dvb.job_offset = offset;
    dvb.job_skip = skip;
    dvb.job_count = helper;
    const uint32_t before = esp_cpu_get_cycle_count();
    __atomic_store_n(&dvb.pending, 1, __ATOMIC_RELEASE);
    xTaskNotifyGive(dvb.helper);
    const uint32_t dispatch = esp_cpu_get_cycle_count()-before;
    if (dispatch > dvb.dispatch_max) dvb.dispatch_max = dispatch;
    const unsigned done = (skip+helper)/12;
    uint16_t *local = dvb.blocks+done+2;
    shaper_moon_pack(local,win,offset+done*(dvb.den == 5 ? 5 : 4),groups-done+2,dvb.moon_mode);
    shaper_moon_sample_tables(dst+helper,local+2,0,count-helper,
                              dvb.moon_tables,dvb.moon_mode);
    const uint32_t join = esp_cpu_get_cycle_count();
    while (__atomic_load_n(&dvb.pending, __ATOMIC_ACQUIRE)) { }
    const uint32_t waited = esp_cpu_get_cycle_count()-join;
    if (waited > dvb.join_max) dvb.join_max = waited;
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
    if (dvb.moon && !dvb.helper_bank_end) {
        const size_t at = first%RADIO_S3_BANK_WORDS;
        dvb.helper_bank_end = first+count+(at && count == RADIO_S3_BANK_WORDS-at ? at : 0);
    }
    const size_t cache_words = dvb.cached_words;
    // radio_s3 calls this once to the physical bank end, then for the wrapped
    // prefix. Save only the suffix that survives that second callback.
    const size_t offset = first % RADIO_S3_BANK_WORDS;
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
    uint8_t *win = dvb.win + (!enc.quads && enc.packed && !dvb.moon ? (first & sps) : 0);
    uint64_t read_first = first*dvb.den, read_count = count*dvb.den;
    unsigned moon_offset = 0;
    if (dvb.moon && count) {
        // Include complete input blocks on both sides. ring_window may round
        // quad starts down by three symbols; retain that prefix in the packer.
        const unsigned block = dvb.den == 5 ? 5 : 4;
        const uint64_t symbol = first/12*block;
        const uint64_t end = (first+count+11)/12*block;
        const uint64_t rounded = enc.quads ? symbol & ~(uint64_t)3 : symbol;
        read_first = symbol*sps;
        read_count = (end-symbol)*sps;
        moon_offset = symbol-rounded;
        if (!enc.quads) win += symbol&1;
    }
    if (count && ring_window(&dvb.ring, win, dvb.win_bytes - 1, read_first, read_count, sps,
                    dvb.history, RADIO_S3_BANK_WORDS*dvb.den+(dvb.moon ? 24*sps : 0)) < 0) return ESP_FAIL;
    const uint32_t copied = esp_cpu_get_cycle_count() - before;
    const uint64_t w0 = dvb.whole.cycles, p0 = dvb.partial.cycles;
    if (dvb.profile) stream_record(&dvb.copy, mark, stream_mark_for(&dvb.copy));
    // A two-frame ring cannot afford a 1 ms polling delay on core 0. Notify only on
    // release, after ring_window's last slot read and release-store of freed. The notification
    // costs this core up to 2.3k cycles, so QPSK's eight-frame ring polls instead, as before.
    uint32_t notified = 0, saved = 0;
    if (dvb.ring.freed != freed && enc.packed && !dvb.moon) {
        const stream_mark_t wake = dvb.profile ? stream_mark_for(&dvb.notify) : (stream_mark_t){0};
        xTaskNotifyGive(enc.task);
        if (dvb.profile) {
            const stream_mark_t after = stream_mark_for(&dvb.notify);
            stream_record(&dvb.notify, wake, after);
            notified = after.cycles - wake.cycles;
        }
    }
    if (dvb.moon) {
        const stream_mark_t whole = dvb.profile ? stream_mark_for(&dvb.whole) : (stream_mark_t){0};
        // History is exactly two input blocks; quad rounding adds a prefix.
        moon_shape(dst,first,count,win,moon_offset);
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
    if (dvb.moon && first+count >= dvb.helper_bank_end) {
        if (dvb.helper_bank > dvb.helper_bank_max) dvb.helper_bank_max = dvb.helper_bank;
        dvb.helper_bank = 0;
        dvb.helper_bank_end = 0;
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
static void encoder_task(void *arg)
{
    encoder_t *e = arg;
    const size_t bytes = (e->mode->kbch - 80) / 8;
    uint8_t *const payload = e->payload;
    uint32_t x = SEED;
    for (uint32_t f = 0; !e->stop; ++f) {
        uint8_t *out;
        while (!(out = ring_slot(&dvb.ring, f))) {
            if (e->stop) {
                goto done;
            }
            e->slot_waits++;
            ulTaskNotifyTake(pdTRUE, e->packed ? portMAX_DELAY : 1);
        }
        const uint32_t start = esp_cpu_get_cycle_count();
        stream_mark_t before = dvb.profile ? stream_mark() : (stream_mark_t){0}, after;
        for (size_t i = 0; i < bytes; ++i) {
            payload[i] = next_byte(&x);
        }
        // Keep station ID and profiling in the interval paired with a future input copy.
        const uint64_t helper_before = dvb.helper_cycles;
        const uint32_t kernel_start = esp_cpu_get_cycle_count();
        station_payload_header(payload, bytes, station_store_get());
        if (dvb.profile) {
            after = stream_mark();
            stream_record(&e->stage[0], before, after);
            before = after;
        }
        int rc;
        if (dvb.profile) {
            rc = dvbs2_fast_bb_gcs(e->fast, e->ws, payload, bytes * 8);
            after = stream_mark(); stream_record(&e->stage[1], before, after); before = after;
            if (!rc) {
                dvbs2_fast_bch(e->fast, e->ws);
                after = stream_mark(); stream_record(&e->stage[2], before, after); before = after;
                dvbs2_fast_ldpc(e->fast, e->ws);
                after = stream_mark(); stream_record(&e->stage[3], before, after); before = after;
                if (e->packed) {
                    dvbs2_fast_map_packed(e->fast, e->ws, out);
                } else {
                    dvbs2_fast_map(e->fast, e->ws, out);
                }
                after = stream_mark(); stream_record(&e->stage[4], before, after); before = after;
                if (e->packed) {
                    dvbs2_fast_pl_packed(e->fast, out);
                } else {
                    dvbs2_fast_pl(e->fast, out);
                }
                after = stream_mark(); stream_record(&e->stage[5], before, after);
            }
        } else if (e->packed) {
            rc = dvbs2_encode_frame_packed(e->fast, e->ws, NULL, payload, bytes * 8, out);
        } else {
            rc = dvbs2_encode_frame(e->fast, e->ws, NULL, payload, bytes * 8, out);
        }
        if (rc) {
            e->failed = true;
            goto done;
        }
        const uint32_t end = esp_cpu_get_cycle_count();
        const uint32_t spent = end - start, kernel_spent = end - kernel_start;
        const uint32_t work = kernel_spent-(uint32_t)(dvb.helper_cycles-helper_before);
        e->work_cycles += work;
        if (f >= dvb.ring.nslots && work > e->work_live_max) e->work_live_max = work;
        ring_publish(&dvb.ring, f);
        e->frames = f + 1;
        e->cycles += spent;
        e->kernel_cycles += kernel_spent;
        e->min_cycles = spent < e->min_cycles ? spent : e->min_cycles;
        e->max_cycles = spent > e->max_cycles ? spent : e->max_cycles;
        if (f >= dvb.ring.nslots && spent > e->live_max_cycles) e->live_max_cycles = spent;
        if (f >= dvb.ring.nslots && kernel_spent > e->kernel_live_max) e->kernel_live_max = kernel_spent;
        if (e->stall_ms) {
            vTaskDelay(pdMS_TO_TICKS(e->stall_ms));
        }
    }
done:
    xSemaphoreGive(e->exited);
    // Keep the notification handle valid even if encoding failed. The command owns
    // deletion after taking exited; a fill may still notify this suspended task.
    vTaskSuspend(NULL);
}

static void release(void)
{
    if (cache_configured) {
        const uint32_t regs[] = {EXTMEM_IBUS_TO_FLASH_START_VADDR_REG,
            EXTMEM_IBUS_TO_FLASH_END_VADDR_REG, EXTMEM_DBUS_TO_FLASH_START_VADDR_REG,
            EXTMEM_DBUS_TO_FLASH_END_VADDR_REG};
        for (unsigned i = 0; i < 4; ++i) REG_WRITE(regs[i], cache_ranges[i]);
        cache_configured = false;
    }
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_free(dvb.moon_extra);
    dvb.moon_extra = NULL;
    heap_caps_free(enc.fast);
    if (!(enc.quads && !dvb.moon && dvb.table.n == 4)) heap_caps_free(enc.ws);
    if (dvb.win && !dvb.moon && (!enc.packed || (enc.mode->bits_per_symbol == 3 && dvb.table.n != 4)))
        heap_caps_free(dvb.win);
    dvb.win = NULL;
    heap_caps_free(enc.bch);
    heap_caps_free(enc.block);
    if (!enc.packed) {
        heap_caps_free(enc.payload);
        heap_caps_free(enc.slots);
        heap_caps_free(enc.filler);
    }
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
    const bool moon = fractional || (mode && mode->bits_per_symbol == 3 && sps == 3);
    const bool short_pulse = !moon && mode && mode->bits_per_symbol == 3 && dac_rate == RADIO_S3_DAC_RATE_80M;
    const bool quads = mode && mode->bits_per_symbol == 2 && dac_rate == RADIO_S3_DAC_RATE_80M;
    if (!mode || !((dac_rate == RADIO_S3_DAC_RATE &&
                    (sps == 4 || sps == 8 || (sps == 40 && mode->bits_per_symbol == 2))) ||
                   (dac_rate == RADIO_S3_DAC_RATE_80M &&
                    (sps == 4 || sps == 3 || fractional)))) {
        printf("@{\"cmd\":\"%s\",\"error\":\"invalid MODCOD/sps\"}\n", cmd);
        return 0;
    }
    const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
    dvb.dac_rate = dac_rate;
    dvb.moon = moon;
    dvb.den = fractional ? 5 : 1;
    dvb.span = moon ? (fractional ? 7 : 6) : 9;
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
    dvb.profile = !strcmp(cmd, "dvbpace") || !strcmp(cmd, "dvbdump");
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
    enc.packed = sps != 40;
    enc.quads = quads;
    dvb.table.n = sps;
    // At 10 Msym/s the 5/6 encoder needs about 69% of a frame period; a two-frame model keeps
    // as little as 244 symbols of lead there, three frames keep 18,871 even at 95% (test_ring).
    const unsigned nslots = !enc.packed ? RING_SLOTS : (sps <= 4 || moon) ? 3 : 2;
    if (moon) {
        dvb.moon_mode = fractional ? (quads ? 3 : 2) : 0;
        dvb.helper_num = 1;
        dvb.helper_den = 3;
        dvb.pending = dvb.encoder_freed = 0;
        dvb.helper_stop = false;
        dvb.helper_cycles = dvb.helper_jobs = dvb.helper_max = 0;
        dvb.helper_bank = dvb.helper_bank_max = dvb.helper_bank_end = 0;
        dvb.dispatch_max = dvb.join_max = 0;
        const unsigned parts = shaper_moon_parts(dvb.moon_mode);
        const size_t words = shaper_moon_words(dvb.moon_mode);
        // Build off the hot path, then distribute whole parts between the
        // existing store and one internal allocation. No cached table reads.
        uint32_t *oracle = heap_caps_aligned_alloc(16,words*4,MALLOC_CAP_SPIRAM);
        const size_t stored = quads ? words : 12288;
        if (words > stored) dvb.moon_extra = heap_caps_aligned_alloc(16,(words-stored)*4,internal);
        if (!oracle || (words > stored && !dvb.moon_extra)) {
            heap_caps_free(oracle);
            release();
            printf("@{\"cmd\":\"%s\",\"error\":\"moon table allocation failed\"}\n",cmd);
            return 0;
        }
        shaper_moon_build(oracle,dvb.moon_mode,level);
        size_t used = 0;
        for (unsigned j = 0; j < parts; ++j) {
            const size_t n = shaper_moon_part_words(dvb.moon_mode,j);
            uint32_t *part = used < stored ? shaper_store+used : dvb.moon_extra+used-stored;
            memcpy(part,oracle+used,n*4);
            dvb.moon_tables[j] = part;
            used += n;
        }
        heap_caps_free(oracle);
        const size_t frame = quads ? ((symbols+3)/4+31) & ~(size_t)15 : packed_bytes(symbols);
        const size_t dummy = quads ? ((DVBS2_DUMMY_SYMBOLS+3)/4+31) & ~(size_t)15 : packed_bytes(DVBS2_DUMMY_SYMBOLS);
        dvb.win_bytes = quads ? 1744 : 6864;
        uint8_t *base = (uint8_t *)shaper_store+stored*4;
        if (stored*4+frame+dummy > sizeof shaper_store) {
            release();
            return 0;
        }
        if (quads) {
            if (stored*4+nslots*frame+dummy+dvb.win_bytes > sizeof shaper_store) {
                release();
                return 0;
            }
            for (unsigned i = 0; i < nslots; ++i) enc.slot[i] = base+i*frame;
            enc.filler = base+nslots*frame;
            dvb.win = enc.filler+dummy;
        } else {
            // The two stored table parts leave room for one frame and dummy.
            enc.block = heap_caps_aligned_alloc(16,2*frame+dvb.win_bytes,internal);
            if (enc.block) {
                enc.slot[0] = base;
                enc.slot[1] = enc.block;
                enc.slot[2] = enc.block+frame;
                enc.filler = base+frame;
                dvb.win = enc.block+2*frame;
            }
        }
    } else if (quads) {
        const size_t frame = ((symbols+3)/4 + 31) & ~(size_t)15;
        const size_t dummy = ((DVBS2_DUMMY_SYMBOLS+3)/4 + 31) & ~(size_t)15;
        const size_t used = QPSK_TABLE_WORDS(sps)*4;
        const unsigned stored = sps == 3 ? 3 : 0;
        uint8_t *base = (uint8_t *)shaper_store + used;
        if (stored < nslots) enc.block = heap_caps_aligned_alloc(16, (nslots-stored)*frame, internal);
        for (unsigned i = 0; i < nslots; ++i)
            enc.slot[i] = i < stored ? base+i*frame : enc.block ? enc.block+(i-stored)*frame : NULL;
        // n4 lends the table tail to the workspace instead of one frame:
        // 12,420 B reclaimed rather than 8,352 B, saving another 4,068 heap bytes.
        const size_t extra = sps == 4 ? (sizeof *enc.ws+15) & ~(size_t)15 : stored*frame;
        enc.filler = base+extra;
        if (used+extra+dummy > sizeof shaper_store) {
            release();
            return 0;
        }
    } else if (enc.packed) {
        const size_t frame = packed_bytes(symbols), dummy = packed_bytes(DVBS2_DUMMY_SYMBOLS);
        const size_t used = short_pulse ? 16384 : shaper_table_words(sps, mode->bits_per_symbol) * 4;
        const size_t need = 2 * frame + dummy + payload_bytes;
        uint8_t *base;
        if (mode->bits_per_symbol == 2) {
            // QPSK's 4/8 KiB table leaves room for all frames, dummy and payload.
            const size_t total = nslots * frame + dummy + payload_bytes;
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
                if (short_pulse) enc.slot[2] = base + ((need + 15) & ~(size_t)15);
                else {
                    enc.slot[2] = enc.block = heap_caps_aligned_alloc(16, frame, internal);
                    if (!enc.block) base = NULL;
                }
            } else {
                base = enc.block = heap_caps_aligned_alloc(16, need, internal);
            }
            if (base) {
                enc.slot[0] = base;
                enc.slot[1] = base + frame;
                enc.filler = base + 2 * frame;
                enc.payload = enc.filler + dummy;
            }
        }
    }
    // Both fast QPSK rates keep workspace and byte BCH off the shared cache.
    const bool resident = enc.packed && (moon || mode->bits_per_symbol == 2 || sps == 4);
    enc.fast = heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode), internal);
    enc.ws = quads && !moon && sps == 4 ? (dvbs2_workspace *)(shaper_store + QPSK_TABLE_WORDS(4)) :
        heap_caps_aligned_alloc(16, sizeof *enc.ws,
                               resident ? internal : MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!moon) dvb.win_bytes = quads ? 2 + (RADIO_S3_BANK_WORDS + 8*sps-2)/(4*sps) + 1 :
                           SHAPER_HISTORY + (RADIO_S3_BANK_WORDS + sps - 1) / sps + 2;
    if (moon) dvb.blocks = enc.ws ? (uint16_t *)((uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED) : NULL;
    if (quads || moon) enc.payload = dvbs2_fast_payload(enc.fast);
    if (!moon) dvb.win = resident ? (enc.ws ? (uint8_t *)enc.ws + DVBS2_FAST_WORKSPACE_USED : NULL) :
                        heap_caps_aligned_alloc(16, dvb.win_bytes, internal);
    if (resident && mode->t > 8)
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
    if (!dvb.win || !enc.fast || !enc.ws || !enc.payload || !enc.filler || !taps ||
            (resident && mode->t > 8 && !enc.bch) || (!enc.packed && !enc.slots) ||
            (quads && !enc.slot[nslots-1]) ||
            dvbs2_fast_prepare(enc.fast, mode, PILOTS,
                              15 | (fractional ? DVBS2_RO_20 : 0) | (quads ? DVBS2_MAP_QUADS : 0) |
                              (resident ? 0 : DVBS2_BCH_NIBBLE | DVBS2_MAP_SPARSE)) ||
            (enc.bch && dvbs2_fast_cache_bch(enc.fast, enc.bch))) {
        printf("@{\"cmd\":\"%s\",\"error\":\"allocation failed\"}\n", cmd);
        heap_caps_free(taps);
        release();
        return 0;
    }
    if (enc.packed) {
        // Pack the dummy PLFRAME through the second slot, before the encoder owns it.
        dvbs2_dummy(enc.slot[1]);
        if (quads) dvbs2_pack_quads(enc.slot[1], enc.filler, DVBS2_DUMMY_SYMBOLS);
        else dvbs2_pack(enc.slot[1], enc.filler, DVBS2_DUMMY_SYMBOLS);
    } else {
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
        ring_cache(&dvb.ring, dvb.overlap, moon ? (quads ? 32 : OVERLAP_SYMBOLS) : SHAPER_HISTORY+2);
        if (quads) dvb.ring.packed = 4;
        else if (!moon && mode->bits_per_symbol == 2) ring_packed_qpsk(&dvb.ring);
        else ring_packed_codes(&dvb.ring);
    } else {
        ring_init(&dvb.ring, enc.slots, RING_SLOTS, enc.filler, symbols);
    }
    ring_filler_length(&dvb.ring, DVBS2_DUMMY_SYMBOLS);

    enc.exited = xSemaphoreCreateBinaryStatic(&exited_storage);
    if (xTaskCreatePinnedToCore(encoder_task, "encoder", 4096, &enc, ENCODER_PRIORITY, &enc.task, 0)
            != pdPASS) {
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder task failed\"}\n", cmd);
        release();
        return 0;
    }
    const TickType_t limit = xTaskGetTickCount() + pdMS_TO_TICKS(nslots * (stall_ms + 100));
    while (__atomic_load_n(&dvb.ring.published, __ATOMIC_ACQUIRE) < nslots && !enc.failed &&
           (int32_t)(xTaskGetTickCount() - limit) < 0) {
        vTaskDelay(1);
    }
    if (__atomic_load_n(&dvb.ring.published, __ATOMIC_ACQUIRE) < nslots) {
        enc.stop = true;
        xTaskNotifyGive(enc.task);
        xSemaphoreTake(enc.exited, portMAX_DELAY);
        vTaskDelete(enc.task);
        printf("@{\"cmd\":\"%s\",\"error\":\"encoder did not fill the ring\"}\n", cmd);
        release();
        return 0;
    }
    if (moon) {
        dvb_helper_exited = xSemaphoreCreateBinaryStatic(&helper_exited_storage);
        if (xTaskCreatePinnedToCore(moon_helper,"fill helper",2048,NULL,ENCODER_PRIORITY+1,&dvb.helper,0) != pdPASS) {
            enc.stop = true;
            xTaskNotifyGive(enc.task);
            xSemaphoreTake(enc.exited,portMAX_DELAY);
            vTaskDelete(enc.task);
            release();
            printf("@{\"cmd\":\"%s\",\"error\":\"fill helper task failed\"}\n",cmd);
            return 0;
        }
    }
    return symbols;
}

// Stop the encoder after a run and print the stream part of the result record.
static void print_profile(const char *name, const stream_profile_t *p)
{
    printf("\"%s\":{\"calls\":%" PRIu32 ",\"cycles\":%" PRIu64 ",\"max_cycles\":%" PRIu32
           ",\"flash_miss\":%" PRIu64 ",\"psram_miss\":%" PRIu64
           ",\"data_access\":%" PRIu64 ",\"instruction_miss\":%" PRIu64 "}",
           name, p->calls, p->cycles, p->max_cycles, p->flash_miss, p->psram_miss,
           p->data_access, p->instruction_miss);
}

static void stop_encoder(void)
{
    if (dvb.moon) {
        dvb.helper_stop = true;
        xTaskNotifyGive(dvb.helper);
        xSemaphoreTake(dvb_helper_exited,portMAX_DELAY);
        vTaskDelete(dvb.helper);
    }
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    enc.stop = true;
    xTaskNotifyGive(enc.task);
    xSemaphoreTake(enc.exited, portMAX_DELAY);
    vTaskDelete(enc.task);
    const ring_t *r = &dvb.ring;
    printf(",\"ring_slots\":%d,\"stall_ms\":%" PRIu32 ",\"frames_encoded\":%" PRIu32 ","
           "\"frames_played\":%" PRIu32 ",\"positions\":%" PRIu32 ",\"underruns\":%" PRIu32 ","
           "\"min_lead_symbols\":%lld,\"encode_cycles_mean\":%" PRIu64 ","
           "\"encode_cycles_min\":%" PRIu32 ",\"encode_cycles_max\":%" PRIu32 ","
           "\"encode_cycles_live_max\":%" PRIu32 ","
           "\"encode_kernel_cycles_mean\":%" PRIu64 ",\"encode_kernel_cycles_live_max\":%" PRIu32 ",\"prefill_frames\":%u,"
           "\"encoder_failed\":%s,\"max_fill_call_cycles\":%" PRIu32 ",\"free_internal\":%u,"
           "\"largest_internal\":%u,\"min_free_internal\":%u",
           (int)r->nslots, enc.stall_ms, enc.frames, r->next_frame, r->next_position, r->underruns,
           r->min_lead == INT64_MAX ? -1LL : (long long)r->min_lead,
           enc.frames ? enc.cycles / enc.frames : 0, enc.frames ? enc.min_cycles : 0,
           enc.max_cycles, enc.live_max_cycles,
           enc.frames ? enc.kernel_cycles / enc.frames : 0, enc.kernel_live_max, (unsigned)r->nslots,
           enc.failed ? "true" : "false", dvb.max_fill_cycles,
           (unsigned)internal, (unsigned)largest, (unsigned)lowest);
    printf(",\"callsign\":\"%s\",\"encoder_timed\":true,\"sps\":%.8g,\"sps_num\":%u,\"sps_den\":%u,\"fill_kernel\":\"%s\"",
           station_store_get(), (double)dvb.table.n/dvb.den, dvb.table.n, dvb.den,
           dvb.moon ? "moon-pie-split" : "pie");
    if (dvb.moon) printf(",\"encode_work_cycles_mean\":%" PRIu64 ",\"encode_work_cycles_live_max\":%" PRIu32,
                         enc.frames ? enc.work_cycles/enc.frames : 0,enc.work_live_max);
    if (dvb.moon) printf(",\"helper_jobs\":%" PRIu32 ",\"helper_cycles\":%" PRIu64
                         ",\"helper_max_cycles\":%" PRIu32 ",\"helper_bank_max_cycles\":%" PRIu32
                         ",\"dispatch_max_cycles\":%" PRIu32 ",\"join_max_cycles\":%" PRIu32
                         ",\"helper_num\":%u,\"helper_den\":%u,\"table_bytes\":%u,\"table_heap_bytes\":%u",
                         dvb.helper_jobs,dvb.helper_cycles,dvb.helper_max,dvb.helper_bank_max,
                         dvb.dispatch_max,dvb.join_max,dvb.helper_num,dvb.helper_den,
                         (unsigned)shaper_moon_words(dvb.moon_mode)*4,
                         dvb.moon_extra ? (unsigned)heap_caps_get_allocated_size(dvb.moon_extra) : 0);
    printf(",\"workspace_memory\":\"%s\",\"workspace_bytes\":%u,\"fast_bytes\":%u,"
           "\"workspace_heap_bytes\":%u,\"fast_heap_bytes\":%u,\"bch_cache_heap_bytes\":%u,"
           "\"bch_cache_bytes\":%u,\"bch_variant\":\"%s\",\"packed_mapper\":\"%s\"",
           enc.packed && (dvb.moon || enc.mode->bits_per_symbol == 2 || dvb.table.n == 4) ? "internal" : "psram", (unsigned)sizeof *enc.ws,
           (unsigned)dvbs2_fast_mode_size(enc.mode),
           enc.quads && !dvb.moon && dvb.table.n == 4 ? 0 : (unsigned)heap_caps_get_allocated_size(enc.ws),
           (unsigned)heap_caps_get_allocated_size(enc.fast),
           enc.bch ? (unsigned)heap_caps_get_allocated_size(enc.bch) : 0,
           enc.bch ? DVBS2_BCH_CACHE_BYTES : 0,
           enc.bch ? "resident-byte" : enc.packed && (dvb.moon || enc.mode->bits_per_symbol == 2 || dvb.table.n == 4)
                                      && enc.mode->t == 8 ? "flash-pair" : "nibble",
           !enc.packed ? "none" : enc.quads ? "quads" : dvb.moon || enc.mode->bits_per_symbol == 2 || dvb.table.n == 4 ? "direct" : "sparse");
    printf(",\"ring_memory\":\"%s\",\"ring_format\":\"%s\",\"ring_heap_bytes\":%u,"
           "\"payload_memory\":\"%s\",\"slot_waits\":%" PRIu32,
           enc.packed ? "internal" : "psram", enc.quads ? "quads" : enc.packed ? "packed" : "raw",
           enc.block ? (unsigned)heap_caps_get_allocated_size(enc.block) : 0,
           enc.packed ? "internal" : "psram", enc.slot_waits);
    printf(",\"layout\":{\"table\":\"0x%" PRIxPTR "\",\"fast\":\"0x%" PRIxPTR
           "\",\"workspace\":\"0x%" PRIxPTR "\",\"payload\":\"0x%" PRIxPTR
           "\",\"filler\":\"0x%" PRIxPTR "\",\"bch\":\"0x%" PRIxPTR "\",\"slots\":[",
           (uintptr_t)dvb.table.blocks, (uintptr_t)enc.fast, (uintptr_t)enc.ws,
           (uintptr_t)enc.payload, (uintptr_t)r->filler, (uintptr_t)enc.bch);
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
                 unsigned sps, uint32_t dac_rate)
{
    const size_t symbols = start_encoder("dvbpace", modcod, level, stall_ms, sps, dac_rate);
    if (!symbols) {
        return;
    }
    radio_s3_stats_t s;
    const esp_err_t err = (dvb.moon ? radio_s3_paced_wifi_rate : radio_s3_paced_run_rate)(seconds, dac_rate, fill, NULL, &s);
    print_mode("dvbpace", modcod, symbols, level);
    printf(",\"paced_wifi_up\":%s",dvb.moon ? "true" : "false");
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
