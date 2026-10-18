#pragma once
#include <stddef.h>
#include <stdint.h>

#define MACHINE_TABLE_WORDS 6144
#define MACHINE_LUT_WORDS 1024
#define MACHINE_PAGE_BLOCKS 128
#define MACHINE_PAGE_SAMPLES (12*MACHINE_PAGE_BLOCKS)
#define MACHINE_PAGE_AXIS_WORDS (5*MACHINE_PAGE_BLOCKS/16)
#define MACHINE_PAGES 54
#define MACHINE_WAKE_PAGES 8
typedef struct {
    uint32_t axes[MACHINE_PAGE_AXIS_WORDS]; // sixteen signs per axis per word
} __attribute__((aligned(16))) machine_page;

// QPSK sign windows, seven-symbol .20 pulse, twelve samples/five symbols.
// src: high-pair-first phase>>1, six history symbols then five symbols/block.
// Prepared words: eleven I-negative bits in 14:4, Q-negative bits in 30:20.
int machine_build(uint32_t *table, unsigned peak);
void machine_lut(uint32_t *lut);
void machine_prepare(uint32_t *dst, const uint8_t *src, unsigned symbol, unsigned blocks,
                     const uint32_t *lut);
void machine_render(uint32_t *dst, const uint32_t *signs, unsigned blocks, const uint32_t *table);
// Expand a compact axis page; return rolling eleven-sign history for the next page.
uint32_t machine_expand(uint32_t *dst, const uint32_t *axes, uint32_t history);
void machine_samples(uint32_t *dst, const uint32_t *signs, unsigned skip, size_t n,
                     const uint32_t *table);
void machine_reference(uint32_t *dst, const uint8_t *src, unsigned symbol,
                       unsigned blocks, const uint32_t *table);
void machine_prepare_pie(uint32_t *, const uint8_t *, unsigned four_blocks, const uint32_t *);
void machine_render_pie(uint32_t *, const uint32_t *, unsigned blocks, const uint32_t *);
