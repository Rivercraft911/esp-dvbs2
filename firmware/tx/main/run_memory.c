// The transmitter's run memory (run_memory.h).
#include "run_memory.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

/* Internal SRAM that the heap never sees, in two link-time regions (ESP-IDF v6.1
 * components/heap/port/esp32s3/memory_layout.c and soc/esp32s3/include/soc/soc.h):
 *   low:  0x3fce0000 up to the ROM's startup stack (SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE),
 *         just above the radio's DAC banks
 *   high: 0x3fcf0000, the half of the data-cache SRAM a 32 KB data cache leaves as DRAM
 * A take goes to the first region with room. PSRAM is one block taken from the heap at boot,
 * before anything else can split it. */
#ifdef ESP_PLATFORM
#include "heap_memory_layout.h"
#include "sdkconfig.h"
_Static_assert(CONFIG_ESP32S3_DATA_CACHE_SIZE == 0x8000, "the high region needs a 32 KB data cache");
SOC_RESERVE_MEMORY_REGION(RUN_LOW_BASE, RUN_LOW_BASE + RUN_LOW_BYTES, run_low);
SOC_RESERVE_MEMORY_REGION(RUN_HIGH_BASE, RUN_HIGH_BASE + RUN_HIGH_BYTES, run_high);
#define LOW ((uint8_t *)RUN_LOW_BASE)
#define HIGH ((uint8_t *)RUN_HIGH_BASE)
#else /* host tests: the same sizes in static memory */
static uint8_t low_store[RUN_LOW_BYTES] __attribute__((aligned(16)));
static uint8_t high_store[RUN_HIGH_BYTES] __attribute__((aligned(16)));
static uint8_t psram_store[RUN_PSRAM_BYTES] __attribute__((aligned(32)));
#define LOW low_store
#define HIGH high_store
#endif

arena_t run_internal[2] = {{LOW, RUN_LOW_BYTES, 0, 0}, {HIGH, RUN_HIGH_BYTES, 0, 0}};
#ifdef ESP_PLATFORM
arena_t run_psram;
#else
arena_t run_psram = {psram_store, RUN_PSRAM_BYTES, 0, 0};
#endif

int run_memory_init(void)
{
    if (run_psram.base) return 0;
    uint8_t *p = heap_caps_aligned_alloc(32, RUN_PSRAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return 1;
    run_psram = (arena_t){p, RUN_PSRAM_BYTES, 0, 0};
    return 0;
}

/* The encoder task may take its payload-input buffers while the command task takes a dump
 * buffer; arena_take_aligned is safe for that. */
static void *take(arena_t *a, size_t bytes, size_t align)
{
    uint8_t *p = a->base ? arena_take_aligned(a, bytes, align) : NULL;
    if (p) memset(p, 0, bytes);
    return p;
}

void *run_take(size_t bytes, size_t align, uint32_t caps)
{
    if (align < 16) align = 16;
    if (caps & MALLOC_CAP_SPIRAM) return take(&run_psram, bytes, align);
    void *p = take(&run_internal[0], bytes, align);
    return p ? p : take(&run_internal[1], bytes, align);
}

void run_reset(void)
{
    __atomic_store_n(&run_internal[0].used, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&run_internal[1].used, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&run_psram.used, 0, __ATOMIC_RELEASE);
}

void run_memory_print(void)
{
    const arena_t *a[] = {&run_internal[0], &run_internal[1], &run_psram};
    static const char *const names[] = {"low", "high", "psram"};
    printf(",\"run_memory\":{");
    for (unsigned i = 0; i < 3; ++i)
        printf("%s\"%s\":[%u,%u,%u]", i ? "," : "", names[i], (unsigned)a[i]->used,
               (unsigned)a[i]->peak, (unsigned)a[i]->size);
    printf("}");
}
