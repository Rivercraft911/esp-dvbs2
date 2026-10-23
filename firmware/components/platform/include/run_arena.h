#pragma once

#include <stddef.h>
#include <stdint.h>

/* Run memory without malloc. Each application reserves its regions when the firmware links; a
 * run takes what it needs from them in order (16-byte aligned) and gives everything back at once
 * when the next run resets them. */
typedef struct {
    uint8_t *base;
    size_t size, used, peak;    /* peak: the most any run has used */
} arena_t;

/* bytes from a, starting at an address aligned to align (a power of two, at least 16) and
 * rounded up to 16 bytes, or NULL if they do not fit (nothing is taken then). One
 * compare-and-swap on the fill level, so two tasks may take from one arena at once. */
static inline void *arena_take_aligned(arena_t *a, size_t bytes, size_t align)
{
    size_t used = __atomic_load_n(&a->used, __ATOMIC_RELAXED), start, end;
    do {
        start = (size_t)((((uintptr_t)a->base + used + align - 1) & ~(uintptr_t)(align - 1)) -
                         (uintptr_t)a->base);
        if (used > a->size || bytes > a->size || start > a->size - bytes) return NULL;
        end = (start + bytes + 15) & ~(size_t)15;
        if (end > a->size) end = a->size;
    } while (!__atomic_compare_exchange_n(&a->used, &used, end, 0, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
    size_t peak = __atomic_load_n(&a->peak, __ATOMIC_RELAXED);
    while (end > peak && !__atomic_compare_exchange_n(&a->peak, &peak, end, 0, __ATOMIC_RELAXED,
                                                      __ATOMIC_RELAXED)) {
    }
    return a->base + start;
}

/* bytes from a, 16-byte aligned, or NULL if they do not fit (nothing is taken then). */
static inline void *arena_take(arena_t *a, size_t bytes)
{
    return arena_take_aligned(a, bytes, 16);
}

/* Heap use since boot ended (arena_booted), by anyone: ESP-IDF's heap hooks count successful
 * allocations and reallocations and frees of non-null pointers, and its failed-allocation
 * callback counts allocations that failed. Zero-size calls, free(NULL), queries and direct
 * multi_heap calls are not seen. Needs CONFIG_HEAP_USE_HOOKS. */
typedef struct {
    uint32_t allocs, frees, failed;
    size_t bytes;               /* allocated in total */
    uint32_t last_size, last_caps;
} arena_heap_t;
void arena_booted(void);
void arena_heap(arena_heap_t *out);
