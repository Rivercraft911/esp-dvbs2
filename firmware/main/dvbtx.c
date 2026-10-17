// Continuous QPSK at 1 Msym/s (40 sps), or 8PSK at 5/10 Msym/s (8/4 sps).
// 8PSK requires an explicit 5m or 10m before duration/frame count in all commands.
// 8PSK keeps its frames in internal RAM, packed two phases per byte (dvbs2.h), so neither core
// touches PSRAM per symbol: at 4 sps three frames, two in the table store's unused half and one
// on the heap; at 8 sps two frames in one heap block (docs/stream-profile.md). QPSK keeps eight
// raw frames in PSRAM.
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
#include "station_id.h"
#include "station_store.h"
#include "station_commands.h"
#include "shaper.h"
#include "shaper_taps.h"

#define RING_SLOTS 8                // QPSK frames the encoder may run ahead: 233 ms at 1 Msym/s
#define PACKED_SLOTS 3              // 8PSK: three packed frames at 4 sps, two at 8, plus a cache
#define PILOTS 1
#define BANK_SYMBOLS 4096           // ceil(16380/4) + 1, worst offset at 4 sps
#define TAPS (9 * 40)
#define WORST_PEAK 718.4            // conservative sqrt(2)*508 LSB axis bound, before rounding
#define SEED 2026
#define ENCODER_PRIORITY 5          // above the command loop, below the Wi-Fi and IPC tasks
// History plus the longest re-read: the bank overlap and the first bank's extra lead, plus one
// symbol at each end that a bank edge may cut (16380 % 8 = 4). 554 symbols at 4 sps, 282 at 8.
#define OVERLAP_SYMBOLS(sps) (SHAPER_HISTORY + 2 + (RADIO_S3_BANK_WORDS - RADIO_S3_STEP_WORDS + \
                              RADIO_S3_LEAD_WORDS + (sps) - 1) / (sps))

typedef struct {
    ring_t ring;
    shaper_table_t table;
    uint8_t win[SHAPER_HISTORY + BANK_SYMBOLS + 1] __attribute__((aligned(16)));  // +1: parity
    uint8_t overlap[OVERLAP_SYMBOLS(4)];
    uint32_t max_fill_cycles;       // one fill call
    bool profile;                  // radio-off commands only
    stream_profile_t copy, whole, partial;
    uint64_t fill_words, fill_cycles;
    uint32_t fill_calls;
    uint32_t worst_copy, worst_whole, worst_partial; // components of the SAME slowest call
} dvb_t;

static DRAM_ATTR dvb_t dvb;

typedef struct {
    const dvbs2_mode *mode;
    dvbs2_fast *fast;               // fast encoder context for this mode, internal RAM
    dvbs2_workspace *ws;            // internal at 4 sps, otherwise cached PSRAM
    uint8_t *bch;                  // resident byte table at 4 sps, t = 10/12
    bool packed;                    // 8PSK: packed internal ring
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
    uint32_t min_cycles, max_cycles;
    stream_profile_t stage[6];      // payload, BB, BCH, LDPC, map, PL
    uint32_t slot_waits;
} encoder_t;

static encoder_t enc;
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

// A whole-symbol start n has n % sps == 0. Since sps = 4, 8 or 40 and
// 16380 are multiples of four, (n % 16380)*4 bytes is 16-byte aligned.
// At 8 sps 16380 % 8 == 4, so a bank wrap can split a symbol in half.
static IRAM_ATTR esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    const uint32_t before = esp_cpu_get_cycle_count();
    const stream_mark_t mark = dvb.profile ? stream_mark_for(&dvb.copy) : (stream_mark_t){0};
    const unsigned sps = dvb.table.n;
    const uint32_t freed = dvb.ring.freed;
    // Packed reads unpack whole vectors when every symbol lands at an address of its own
    // parity (ring.h); the history is even, so shift the window by the first symbol's parity.
    // sps is 4 or 8 there, so that parity is bit log2(sps) of first. QPSK keeps the window.
    uint8_t *const win = dvb.win + (enc.packed && (first & sps));
    if (ring_window(&dvb.ring, win, sizeof dvb.win - 1, first, count, sps,
                    SHAPER_HISTORY, RADIO_S3_BANK_WORDS) < 0) {
        return ESP_FAIL;
    }
    // A two-frame ring cannot afford a 1 ms polling delay on core 0. Notify only on
    // release, after ring_window's last slot read and release-store of freed. The notification
    // costs this core up to 2.3k cycles, so QPSK's eight-frame ring polls instead, as before.
    if (dvb.ring.freed != freed && enc.packed) xTaskNotifyGive(enc.task);
    const uint32_t copied = esp_cpu_get_cycle_count() - before;
    const uint64_t w0 = dvb.whole.cycles, p0 = dvb.partial.cycles;
    if (dvb.profile) stream_record(&dvb.copy, mark, stream_mark_for(&dvb.copy));
    shaper_samples_profile(dst, win + SHAPER_HISTORY, first % sps, count, &dvb.table,
                           dvb.profile ? &dvb.whole : NULL,
                           dvb.profile ? &dvb.partial : NULL);
    const uint32_t cycles = esp_cpu_get_cycle_count() - before;
    dvb.fill_words += count;
    dvb.fill_cycles += cycles;
    dvb.fill_calls++;
    if (cycles > dvb.max_fill_cycles) {
        dvb.max_fill_cycles = cycles;
        dvb.worst_copy = copied;
        dvb.worst_whole = dvb.whole.cycles - w0;
        dvb.worst_partial = dvb.partial.cycles - p0;
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
    heap_caps_free(enc.fast);
    heap_caps_free(enc.ws);
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
    memset(&dvb.copy, 0, sizeof dvb.copy);
    memset(&dvb.whole, 0, sizeof dvb.whole);
    memset(&dvb.partial, 0, sizeof dvb.partial);
    dvb.fill_words = dvb.fill_cycles = dvb.fill_calls = 0;
    dvb.worst_copy = dvb.worst_whole = dvb.worst_partial = 0;
    dvb.profile = !strcmp(cmd, "dvbpace") || !strcmp(cmd, "dvbdump");
    // The fill phases of a timed run record cycles only; an untimed dump also reads the
    // shared cache counters. Encoder stages always read them.
    dvb.copy.counters = dvb.whole.counters = dvb.partial.counters = !strcmp(cmd, "dvbdump");
    if (dvb.profile) cache_profile_start();
    enc.stall_ms = stall_ms;
    enc.min_cycles = UINT32_MAX;
    for (unsigned i = 0; i < 6; ++i) {
        enc.stage[i].counters = 1;
    }
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t payload_bytes = (mode->kbch - 80) / 8;
    enc.packed = mode->bits_per_symbol == 3;
    // At 10 Msym/s the 5/6 encoder needs about 69% of a frame period; a two-frame model keeps
    // as little as 244 symbols of lead there, three frames keep 18,871 even at 95% (test_ring).
    const unsigned nslots = !enc.packed ? RING_SLOTS : sps == 4 ? 3 : 2;
    if (enc.packed) {
        // Two packed frames, the packed dummy and the payload, 16-byte aligned (31,200 B at
        // most, for MODCOD 17). The 4-sps table uses the store's first 32 KiB, so they go in the
        // rest and the third frame on the heap. At 8 sps the table fills the store, so all of
        // it is one heap block. Heap parts are made before the context.
        const size_t frame = packed_bytes(symbols), dummy = packed_bytes(DVBS2_DUMMY_SYMBOLS);
        const size_t need = 2 * frame + dummy + payload_bytes;
        uint8_t *base;
        if (sps == 4) {
            const size_t used = shaper_table_words(sps, mode->bits_per_symbol) * 4;
            if (used + need > sizeof shaper_store) {
                printf("@{\"cmd\":\"%s\",\"error\":\"internal ring does not fit\"}\n", cmd);
                release();
                return 0;
            }
            base = (uint8_t *)shaper_store + used;
            enc.slot[2] = enc.block = heap_caps_aligned_alloc(16, frame, internal);
        } else {
            base = enc.block = heap_caps_aligned_alloc(16, need, internal);
        }
        if (enc.block) {
            enc.slot[0] = base;
            enc.slot[1] = base + frame;
            enc.filler = base + 2 * frame;
            enc.payload = enc.filler + dummy;
        }
    }
    // 4-sps 8PSK: keep the FEC workspace and byte BCH table off the shared data
    // cache. The 8-sps table needs the whole store, so retain its old heap policy.
    const bool resident = enc.packed && sps == 4;
    enc.fast = heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode), internal);
    enc.ws = heap_caps_aligned_alloc(16, sizeof *enc.ws,
                                    resident ? internal : MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    if (!enc.fast || !enc.ws || !enc.payload || !enc.filler || !taps ||
            (resident && mode->t > 8 && !enc.bch) || (!enc.packed && !enc.slots) ||
            dvbs2_fast_prepare(enc.fast, mode, PILOTS, 15 | (resident ? 0 : DVBS2_BCH_NIBBLE)) ||
            (enc.bch && dvbs2_fast_cache_bch(enc.fast, enc.bch))) {
        printf("@{\"cmd\":\"%s\",\"error\":\"allocation failed\"}\n", cmd);
        heap_caps_free(taps);
        release();
        return 0;
    }
    if (enc.packed) {
        // Pack the dummy PLFRAME through the second slot, before the encoder owns it.
        dvbs2_dummy(enc.slot[1]);
        dvbs2_pack(enc.slot[1], enc.filler, DVBS2_DUMMY_SYMBOLS);
    } else {
        dvbs2_dummy(enc.filler);
    }
    // Scale the conservative envelope to level; table rounding adds at most 1.5 LSB/axis.
    const int32_t *source = sps == 40 ? shaper_taps_qpsk_n40_a35 :
                            sps == 8 ? shaper_taps_8psk_n8_a35 : shaper_taps_8psk_n4_a35;
    for (unsigned i = 0; i < 9 * sps; ++i) {
        taps[i] = (int32_t)((double)source[i] * level / WORST_PEAK);
    }
    shaper_build(&dvb.table, shaper_store, taps, sps, mode->bits_per_symbol);
    heap_caps_free(taps);
    dvb.max_fill_cycles = 0;
    if (enc.packed) {
        ring_init_slots(&dvb.ring, enc.slot, nslots, enc.filler, symbols);
        ring_cache(&dvb.ring, dvb.overlap, OVERLAP_SYMBOLS(sps));
        ring_packed(&dvb.ring);
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
           "\"encoder_failed\":%s,\"max_fill_call_cycles\":%" PRIu32 ",\"free_internal\":%u,"
           "\"largest_internal\":%u,\"min_free_internal\":%u",
           (int)r->nslots, enc.stall_ms, enc.frames, r->next_frame, r->next_position, r->underruns,
           r->min_lead == INT64_MAX ? -1LL : (long long)r->min_lead,
           enc.frames ? enc.cycles / enc.frames : 0, enc.frames ? enc.min_cycles : 0,
           enc.max_cycles, enc.failed ? "true" : "false", dvb.max_fill_cycles,
           (unsigned)internal, (unsigned)largest, (unsigned)lowest);
    printf(",\"callsign\":\"%s\"", station_store_get());
    printf(",\"workspace_memory\":\"%s\",\"workspace_bytes\":%u,\"fast_bytes\":%u,"
           "\"bch_cache_bytes\":%u,\"bch_variant\":\"%s\"",
           enc.packed && dvb.table.n == 4 ? "internal" : "psram", (unsigned)sizeof *enc.ws,
           (unsigned)dvbs2_fast_mode_size(enc.mode), enc.bch ? DVBS2_BCH_CACHE_BYTES : 0,
           enc.bch ? "resident-byte" : "nibble");
    printf(",\"ring_memory\":\"%s\",\"ring_format\":\"%s\",\"ring_heap_bytes\":%u,"
           "\"payload_memory\":\"%s\",\"slot_waits\":%" PRIu32,
           enc.packed ? "internal" : "psram", enc.packed ? "packed" : "raw",
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
               ",\"partial\":%" PRIu32 "},\"fill\":{",
               dvb.copy.counters ? "true" : "false", dvb.fill_calls, dvb.fill_words,
               dvb.fill_cycles, dvb.worst_copy, dvb.worst_whole, dvb.worst_partial);
        print_profile("copy", &dvb.copy); printf(",");
        print_profile("whole", &dvb.whole); printf(",");
        print_profile("partial", &dvb.partial); printf("},\"encoder\":{");
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
                unsigned sps)
{
    if (!station_require("dvb")) {
        return;
    }
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
    print_wifi(&s.wifi);
    stop_encoder();
    printf(",\"status\":\"%s\"%s}\n", esp_err_to_name(err),
           err == ESP_ERR_INVALID_ARG ? ",\"error\":\"duration too short for start/end ID\"" :
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
           "\"sample_rate\":%u}\n", bytes, RADIO_S3_DAC_RATE / decim);
    fflush(stdout);
    radio_s3_dry_stats_t s;
    const esp_err_t err = radio_s3_dry_run(fill, NULL, samples, sink, d, &s);
    if (d->used) {
        flush(d);
    }
    const uint32_t crc = d->crc;
    heap_caps_free(d);
    print_mode("dvbdump", modcod, symbols, level);
    printf(",\"frames\":%u,\"decimation\":%u,\"samples\":%" PRIu64 ",\"crc32\":%" PRIu32 ","
           "\"fills\":%" PRIu32 ",\"max_fill_cycles\":%" PRIu32 ",\"overlap_words\":%" PRIu32 ","
           "\"overlap_mismatches\":%" PRIu32,
           frames, decim, samples, crc, s.fills, s.max_fill_cycles, s.overlap_words,
           s.overlap_mismatches);
    stop_encoder();
    printf(",\"status\":\"%s\"}\n", esp_err_to_name(err));
}

void dvbtx_wifi(unsigned modcod, unsigned sps)
{
    if (!station_require("wifimem")) {
        return;
    }
    size_t symbols = 0;
    if (modcod) {
        symbols = start_encoder("wifimem", modcod, 128, 0, sps);
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
