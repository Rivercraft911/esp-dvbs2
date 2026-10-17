#pragma once

// Radio-off diagnostics. Unsigned deltas tolerate one 32-bit counter wrap per stage.
// EXTMEM counts both CPUs together, including interrupts, flash data and PSRAM.
// These are overlapping observation windows, not exclusive per-core attribution.
// Register definitions: IDF v6.1 soc/esp32s3/register/soc/extmem_reg.h, offsets C8..D8.
#include <stdint.h>

#ifdef __XTENSA__
#include "esp_cpu.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"
#endif

typedef struct {
    uint32_t cycles, flash_miss, psram_miss, data_access, instruction_miss;
} stream_mark_t;

typedef struct {
    uint64_t cycles, flash_miss, psram_miss, data_access, instruction_miss;
    uint32_t calls, max_cycles;
    uint8_t counters;   // also read the EXTMEM counters at each mark (stream_mark_for)
} stream_profile_t;

static inline stream_mark_t stream_mark(void)
{
#ifdef __XTENSA__
    // Take CCOUNT last: the next interval includes the next snapshot's register reads.
    stream_mark_t m;
    m.flash_miss = REG_READ(EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG);
    m.psram_miss = REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG);
    m.data_access = REG_READ(EXTMEM_DBUS_ACS_CNT_REG);
    m.instruction_miss = REG_READ(EXTMEM_IBUS_ACS_MISS_CNT_REG);
    m.cycles = esp_cpu_get_cycle_count();
    return m;
#else
    return (stream_mark_t){0};
#endif
}

static inline void stream_record(stream_profile_t *p, stream_mark_t a, stream_mark_t b)
{
    const uint32_t cycles = b.cycles - a.cycles;
    p->cycles += cycles;
    if (p->counters) {
        p->flash_miss += (uint32_t)(b.flash_miss - a.flash_miss);
        p->psram_miss += (uint32_t)(b.psram_miss - a.psram_miss);
        p->data_access += (uint32_t)(b.data_access - a.data_access);
        p->instruction_miss += (uint32_t)(b.instruction_miss - a.instruction_miss);
    }
    p->calls++;
    if (cycles > p->max_cycles) p->max_cycles = cycles;
}

// CCOUNT only. The four EXTMEM reads of stream_mark cost the fill 0.9k to 1.6k cycles per bank
// on the board (results/2026-10-16-buildout), so timed runs mark fill phases this way.
static inline stream_mark_t stream_mark_cycles(void)
{
#ifdef __XTENSA__
    return (stream_mark_t){.cycles = esp_cpu_get_cycle_count()};
#else
    return (stream_mark_t){0};
#endif
}

static inline stream_mark_t stream_mark_for(const stream_profile_t *p)
{
    return p->counters ? stream_mark() : stream_mark_cycles();
}
