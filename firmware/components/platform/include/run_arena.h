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
