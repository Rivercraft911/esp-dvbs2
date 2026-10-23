#pragma once

#include <stddef.h>
#include <stdint.h>
#include "run_arena.h"

/* The transmitter's run memory, in one place. Every command that needs memory for one run
 * (dvb, dvbpace, dvbdump, fecbench, iddump, spisend/spirx, payload input) takes it here; the
 * next command's run_reset() gives all of it back. Nothing comes from the heap after boot.
 *   run_internal[0], [1]: internal SRAM the heap never sees (run_memory.c), for what the
 *                         encoder, the bank fill and DMA touch
 *   run_psram:            PSRAM, for cold tables, payload staging and dump buffers */
extern arena_t run_internal[2], run_psram;
#define RUN_LOW_BASE 0x3fce0000U
#define RUN_LOW_BYTES (0x3fceb710U - 0x2000U - RUN_LOW_BASE) /* to the ROM's startup stack */
#define RUN_HIGH_BASE 0x3fcf0000U
#define RUN_HIGH_BYTES 0x8000U
#define RUN_PSRAM_BYTES (384U << 10)

/* bytes aligned to align (16 or 32), zeroed: from PSRAM if caps has MALLOC_CAP_SPIRAM, otherwise
 * from the first internal region with room. NULL if it does not fit (nothing is taken then). */
void *run_take(size_t bytes, size_t align, uint32_t caps);
void run_reset(void);
/* ,"run_memory":{"low":[used,peak,size],"high":[...],"psram":[...]} for a JSON record. */
void run_memory_print(void);
/* Once at boot, before the heap is split: takes the PSRAM block. Nonzero on failure. */
int run_memory_init(void);
