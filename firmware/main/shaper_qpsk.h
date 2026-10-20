#pragma once

#include <stddef.h>
#include <stdint.h>

// Four QPSK phases per byte, oldest in bits 7:6, newest in 1:0; code = phase k >> 1.
// Phase k is 1,3,5,7 for exp(j*pi*k/4). Blocks follow the global symbol timeline, not
// frame boundaries (33282 symbols/frame is 2 mod 4). src[-2..-1] is eight-symbol history.
// Three parts, 256 rows/part, 4*n DAC words/row; no padding. n is 3 or 4 only.
#define QPSK_TABLE_WORDS(n) (3u * 256u * 4u * (n))
#define QPSK_HISTORY_BYTES 2

// 16-byte aligned store; level 16..508 DAC LSB. Returns -1 on invalid arguments.
// Two LSB reserved for table rounding.
int shaper_qpsk_build(uint32_t *table, unsigned n, unsigned level);
void shaper_qpsk_c(uint32_t *dst, const uint8_t *src, unsigned groups,
                   const uint32_t *table, unsigned n);
// dst/table must be 16-byte aligned. Zero groups does not read any buffer.
void shaper_qpsk3_pie(uint32_t *, const uint8_t *, unsigned, const uint32_t *);
void shaper_qpsk4_pie(uint32_t *, const uint8_t *, unsigned, const uint32_t *);
// Samples starting at skip (0..4*n-1) in src[0]'s block. Only required blocks are read.
// dst + (4*n-skip) must be vector aligned for PIE bulk; otherwise uses C scratch.
// Writes exactly count words, including empty and partial calls.
void shaper_qpsk_samples(uint32_t *dst, const uint8_t *src, unsigned skip, size_t count,
                         const uint32_t *table, unsigned n);
