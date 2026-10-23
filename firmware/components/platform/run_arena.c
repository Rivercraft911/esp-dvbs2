// The heap-call counter behind run memory (run_arena.h).
#include "run_arena.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

/* ESP-IDF calls these after each successful allocation or reallocation and each free of a
 * non-null pointer (CONFIG_HEAP_USE_HOOKS), and the failed-allocation callback once registered.
 * In IRAM, since the heap's own code may be there and run with the flash cache off. */
static volatile int booted;
static arena_heap_t heap;

void IRAM_ATTR esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    if (!booted || !ptr) return;
    __atomic_fetch_add(&heap.allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&heap.bytes, size, __ATOMIC_RELAXED);
    __atomic_store_n(&heap.last_size, (uint32_t)size, __ATOMIC_RELAXED);
    __atomic_store_n(&heap.last_caps, caps, __ATOMIC_RELAXED);
}

void IRAM_ATTR esp_heap_trace_free_hook(void *ptr)
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
