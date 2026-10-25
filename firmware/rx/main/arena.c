// The receiver's link-time run memory (arena.h).
#include "arena.h"

#include "dvbs2rx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "heap_memory_layout.h"

/* What a run takes, at its largest mode (sizes from rx_*_size):
 *   low:   demodulator (15.1 KB) + clean-path FEC (10.0 to 12.3 KB) + TS deframer (0.8 KB)
 *   high:  symbol ring (16 KB) + three frame slots of hard and weak bits (11.9 KB); membench
 *          borrows 16 KB of it
 *   psram: decoder state and work (66 to 81 KB) + the decoder's frame (4 KB) + CRC-32 table
 *   fast:  the decoder's per-layer scratch (4.4 to 13.6 KB): in PSRAM it made a decode 1.1 to
 *          2.2 times slower (results/2026-10-24-rx-fec-curve) */
#define LOW_BYTES (30U << 10)
#define HIGH_BASE 0x3fce0000U       /* the first internal RAM above the banks (capture.h) */
#define HIGH_BYTES (28U << 10)
#define PSRAM_BYTES (96U << 10)

static uint8_t low[LOW_BYTES] __attribute__((aligned(16)));
SOC_RESERVE_MEMORY_REGION(HIGH_BASE, HIGH_BASE + HIGH_BYTES, rx_arena);
static EXT_RAM_BSS_ATTR uint8_t psram[PSRAM_BYTES] __attribute__((aligned(16)));
EXT_RAM_BSS_ATTR int8_t arena_store[ARENA_STORE_BYTES] __attribute__((aligned(16)));

arena_t arena_low = {low, LOW_BYTES, 0, 0};
arena_t arena_high = {(uint8_t *)HIGH_BASE, HIGH_BYTES, 0, 0};
arena_t arena_psram = {psram, PSRAM_BYTES, 0, 0};
arena_t arena_fast;

static size_t round16(size_t n) { return (n + 15) & ~(size_t)15; }

static const uint8_t modcods[] = {4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16};

void arena_init(void)
{
    size_t most = 0;
    for (unsigned k = 0; k < sizeof modcods; ++k) {
        const size_t n = round16(rx_fec_scratch_size(modcods[k]));
        most = n > most ? n : most;
    }
    uint8_t *base = heap_caps_aligned_alloc(16, most, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    arena_fast = (arena_t){base, base ? most : 0, 0, 0};
}

void arena_reset(void)
{
    arena_low.used = arena_high.used = arena_psram.used = arena_fast.used = 0;
}

int arena_check(void)
{
    int bad = round16(RX_DEMOD_RING_BYTES) + 2 * 3 * round16(RX_HARD_BYTES) > HIGH_BYTES;
    for (unsigned k = 0; k < sizeof modcods; ++k) {
        const unsigned m = modcods[k];
        bad |= round16(rx_demod_size()) + round16(rx_fec_check_size(m)) + round16(rx_ts_size()) > LOW_BYTES;
        bad |= round16(rx_fec_size(m)) + round16(rx_fec_work_size(m)) + 2 * round16(RX_HARD_BYTES) +
               1024 > PSRAM_BYTES;
        bad |= round16(rx_fec_scratch_size(m)) > arena_fast.size;
    }
    return bad;
}
