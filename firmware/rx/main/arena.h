#pragma once

#include <stddef.h>
#include <stdint.h>
#include "run_arena.h"

/* The receiver's run memory (run_arena.h). Three regions are reserved when the firmware links:
 *   arena_low:   static internal RAM below the capture banks (static DRAM must end at 0x3fcb0000)
 *   arena_high:  internal RAM just above the banks, taken from the heap at link time
 *   arena_psram: static PSRAM
 * They are sized for the largest mode (arena.c), which arena_check() confirms at boot. */
extern arena_t arena_low, arena_high, arena_psram;

void arena_reset(void);
/* Whether every mode's run fits; 0 when it does. */
int arena_check(void);

/* The capture store in PSRAM: a replay's uploaded capture or a dump's samples, one at a time. */
#define ARENA_STORE_BYTES (7U << 20)
extern int8_t arena_store[ARENA_STORE_BYTES];
