#pragma once
#include <stddef.h>
#include <stdint.h>

#define MACHINE_TABLE_WORDS 6144
#define MACHINE_PAGE_BLOCKS 128
#define MACHINE_PAGE_SAMPLES (12*MACHINE_PAGE_BLOCKS)
#define MACHINE_PAGE_AXIS_WORDS (5*MACHINE_PAGE_BLOCKS/16)
#define MACHINE_PAGES 105
#define MACHINE_WAKE_PAGES 8
#define MACHINE_AXIS_CHUNK_SAMPLES 192
typedef struct {
    uint32_t axes[MACHINE_PAGE_AXIS_WORDS]; // sixteen signs per axis per word
} __attribute__((aligned(16))) machine_page;

// QPSK sign windows, seven-symbol .20 pulse, twelve samples/five symbols.
int machine_build(uint32_t *table, unsigned peak);
// Direct axis rendering within one page, n <= MACHINE_PAGE_SAMPLES-skip.
// scratch is 192 words aligned to 16 bytes for bank-cut fragments.
void machine_axis_samples(uint32_t *dst, const uint32_t *axes, uint32_t previous,
                          unsigned skip, size_t n, const uint32_t *table, uint32_t *scratch);
void machine_axis_render_pie(uint32_t *, const uint32_t *, unsigned runs,
                             uint32_t previous, const uint32_t *table);
#ifndef __XTENSA__
// Host oracles. Signs: eleven I-negative bits in 14:4, Q-negative bits in 30:20.
// src: high-pair-first phase>>1, six history symbols then five symbols/block.
void machine_render(uint32_t *dst, const uint32_t *signs, unsigned blocks, const uint32_t *table);
// Expand a compact axis page; return rolling eleven-sign history for the next page.
uint32_t machine_expand(uint32_t *dst, const uint32_t *axes, uint32_t history);
void machine_reference(uint32_t *dst, const uint8_t *src, unsigned symbol,
                       unsigned blocks, const uint32_t *table);
#endif
