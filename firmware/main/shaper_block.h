#pragma once

// Experiment: four 3-sample symbols per output block. Not a transmit mode.
// Three independent 3-symbol tables, each 512 rows of 12 words (24 KiB).
#include <stdint.h>
#define SHAPER_BLOCK_WORDS (512 * 12)
void shaper_block_build(uint32_t *tables[3]);
// Four phases per uint16_t, newest in bits 2:0. src[-2..-1] provides history.
// Output and each table must be 16-byte aligned. Exactly 12*groups words written.
void shaper_block_c(uint32_t *dst, const uint16_t *src, unsigned groups,
                    const uint32_t *t0, const uint32_t *t1, const uint32_t *t2);
void shaper_block_pie(uint32_t *dst, const uint16_t *src, unsigned groups,
                      const uint32_t *t0, const uint32_t *t1, const uint32_t *t2);
