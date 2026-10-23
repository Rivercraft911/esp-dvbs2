#pragma once

#include <stddef.h>
#include <stdint.h>

/* Run memory without malloc. Three regions are reserved when the firmware links; a run takes
 * what it needs from them in order (16-byte aligned) and gives everything back at once when
 * the next run resets them.
 *   arena_low:   static internal RAM below the capture banks (static DRAM must end at 0x3fcb0000)
 *   arena_high:  internal RAM just above the banks, taken from the heap at link time
 *   arena_psram: static PSRAM
 * They are sized for the largest mode (arena.c), which arena_check() confirms at boot. */
typedef struct {
    uint8_t *base;
    size_t size, used, peak;    /* peak: the most any run has used */
} arena_t;

extern arena_t arena_low, arena_high, arena_psram;

/* bytes from a, 16-byte aligned, or NULL if they do not fit (nothing is taken then). */
static inline void *arena_take(arena_t *a, size_t bytes)
{
    if (bytes > SIZE_MAX - 15 || a->used > a->size) return NULL;
    bytes = (bytes + 15) & ~(size_t)15;
    if (bytes > a->size - a->used) return NULL;
    void *p = a->base + a->used;
    a->used += bytes;
    if (a->used > a->peak) a->peak = a->used;
    return p;
}

void arena_reset(void);
/* Whether every mode's run fits; 0 when it does. */
int arena_check(void);

/* The capture store in PSRAM: a replay's uploaded capture or a dump's samples, one at a time. */
#define ARENA_STORE_BYTES (7U << 20)
extern int8_t arena_store[ARENA_STORE_BYTES];

/* Heap use since boot ended (arena_booted), by anyone: ESP-IDF's heap hooks count successful
 * allocations and reallocations and frees of non-null pointers, and its failed-allocation
 * callback counts allocations that failed. Zero-size calls, free(NULL), queries and direct
 * multi_heap calls are not seen. */
typedef struct {
    uint32_t allocs, frees, failed;
    size_t bytes;               /* allocated in total */
    uint32_t last_size, last_caps;
} arena_heap_t;
void arena_booted(void);
void arena_heap(arena_heap_t *out);
