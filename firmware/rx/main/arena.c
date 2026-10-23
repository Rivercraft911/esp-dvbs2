// Link-time run memory and the heap-call counter (arena.h).
#include "arena.h"

#include "dvbs2rx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "heap_memory_layout.h"

/* What a run takes, at its largest mode (sizes from rx_*_size):
 *   low:   demodulator (15.1 KB) + clean-path FEC (10.0 to 12.3 KB) + TS deframer (0.8 KB)
 *   high:  symbol ring (16 KB) + three frame slots of hard and weak bits (11.9 KB); membench
 *          borrows 16 KB of it
 *   psram: decoder state and work (66 to 81 KB) + the decoder's frame (4 KB) + CRC-32 table */
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

static size_t round16(size_t n) { return (n + 15) & ~(size_t)15; }

void arena_reset(void)
{
    arena_low.used = arena_high.used = arena_psram.used = 0;
}

int arena_check(void)
{
    static const uint8_t modcods[] = {4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16};
    int bad = round16(RX_DEMOD_RING_BYTES) + 2 * 3 * round16(RX_HARD_BYTES) > HIGH_BYTES;
    for (unsigned k = 0; k < sizeof modcods; ++k) {
        const unsigned m = modcods[k];
        bad |= round16(rx_demod_size()) + round16(rx_fec_check_size(m)) + round16(rx_ts_size()) > LOW_BYTES;
        bad |= round16(rx_fec_size(m)) + round16(rx_fec_work_size(m)) + 2 * round16(RX_HARD_BYTES) +
               1024 > PSRAM_BYTES;
    }
    return bad;
}

/* ESP-IDF calls these after each successful allocation or reallocation and each free of a
 * non-null pointer (CONFIG_HEAP_USE_HOOKS), and the failed-allocation callback once registered. */
static volatile int booted;
static arena_heap_t heap;

void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    if (!booted || !ptr) return;
    __atomic_fetch_add(&heap.allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&heap.bytes, size, __ATOMIC_RELAXED);
    __atomic_store_n(&heap.last_size, (uint32_t)size, __ATOMIC_RELAXED);
    __atomic_store_n(&heap.last_caps, caps, __ATOMIC_RELAXED);
}

void esp_heap_trace_free_hook(void *ptr)
{
    if (booted && ptr) __atomic_fetch_add(&heap.frees, 1, __ATOMIC_RELAXED);
}

static void failed(size_t size, uint32_t caps, const char *function)
{
    (void)function;
    __atomic_fetch_add(&heap.failed, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&heap.last_size, (uint32_t)size, __ATOMIC_RELAXED);
    __atomic_store_n(&heap.last_caps, caps, __ATOMIC_RELAXED);
}

void arena_booted(void)
{
    heap_caps_register_failed_alloc_callback(failed);
    booted = 1;
}

void arena_heap(arena_heap_t *out)
{
    out->allocs = __atomic_load_n(&heap.allocs, __ATOMIC_RELAXED);
    out->frees = __atomic_load_n(&heap.frees, __ATOMIC_RELAXED);
    out->failed = __atomic_load_n(&heap.failed, __ATOMIC_RELAXED);
    out->bytes = __atomic_load_n(&heap.bytes, __ATOMIC_RELAXED);
    out->last_size = __atomic_load_n(&heap.last_size, __ATOMIC_RELAXED);
    out->last_caps = __atomic_load_n(&heap.last_caps, __ATOMIC_RELAXED);
}
