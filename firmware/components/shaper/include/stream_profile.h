#pragma once

// Radio-off diagnostics. Unsigned deltas tolerate one 32-bit counter wrap per stage.
// EXTMEM counts both CPUs together, including interrupts, flash data and PSRAM.
// These are overlapping observation windows, not exclusive per-core attribution.
// Register definitions: IDF v6.1 soc/esp32s3/register/soc/extmem_reg.h, offsets C8..D8.
#include <stdint.h>
#ifdef STREAM_PROFILE_HOST
#include "esp_cpu.h"
#endif

#ifdef __XTENSA__
#include "esp_cpu.h"
#include "esp_attr.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"
#endif

typedef struct {
    uint32_t cycles, flash_miss, psram_miss, data_access, instruction_miss;
} stream_mark_t;

typedef struct {
    uint64_t cycles, flash_miss, psram_miss, data_access, instruction_miss;
    uint64_t prepare_cycles; // encoder windows: synchronous queue preparation
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
#elif defined(STREAM_PROFILE_HOST)
    return (stream_mark_t){.cycles = esp_cpu_get_cycle_count()};
#else
    return (stream_mark_t){0};
#endif
}

// Keep the repeated 64-bit accumulation out of every fill branch. IRAM shares
// physical SRAM with the heap on the S3, even when that branch is never used.
#ifndef STREAM_PROFILE_FN
#ifdef __XTENSA__
#define STREAM_PROFILE_FN static IRAM_ATTR __attribute__((noinline, unused))
#else
#define STREAM_PROFILE_FN static inline
#endif
#endif
STREAM_PROFILE_FN void stream_record(stream_profile_t *p, stream_mark_t a, stream_mark_t b)
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
#elif defined(STREAM_PROFILE_HOST)
    return (stream_mark_t){.cycles = esp_cpu_get_cycle_count()};
#else
    return (stream_mark_t){0};
#endif
}

STREAM_PROFILE_FN stream_mark_t stream_mark_for(const stream_profile_t *p)
{
    return p->counters ? stream_mark() : stream_mark_cycles();
}

// The same through pointers, for the encoder's per-stage marks: by-value marks cost four
// ROM memcpy calls per stage on Xtensa.
static inline __attribute__((always_inline)) void stream_mark_at(stream_mark_t *m, int counters)
{
#ifdef __XTENSA__
    if (counters) {
        m->flash_miss = REG_READ(EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG);
        m->psram_miss = REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG);
        m->data_access = REG_READ(EXTMEM_DBUS_ACS_CNT_REG);
        m->instruction_miss = REG_READ(EXTMEM_IBUS_ACS_MISS_CNT_REG);
    } else m->flash_miss = m->psram_miss = m->data_access = m->instruction_miss = 0;
    m->cycles = esp_cpu_get_cycle_count();
#else
    (void)counters;
    m->flash_miss = m->psram_miss = m->data_access = m->instruction_miss = 0;
    m->cycles = stream_mark_cycles().cycles;
#endif
}

STREAM_PROFILE_FN void stream_record_at(stream_profile_t *p, const stream_mark_t *a, const stream_mark_t *b)
{
    const uint32_t cycles = b->cycles - a->cycles;
    p->cycles += cycles;
    if (p->counters) {
        p->flash_miss += (uint32_t)(b->flash_miss - a->flash_miss);
        p->psram_miss += (uint32_t)(b->psram_miss - a->psram_miss);
        p->data_access += (uint32_t)(b->data_access - a->data_access);
        p->instruction_miss += (uint32_t)(b->instruction_miss - a->instruction_miss);
    }
    p->calls++;
    if (cycles > p->max_cycles) p->max_cycles = cycles;
}

#undef STREAM_PROFILE_FN
