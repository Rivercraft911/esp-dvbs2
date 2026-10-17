#pragma once
#include <stddef.h>
#include <stdint.h>

#define MACHINE_TABLE_WORDS 6912
#define MACHINE_PAGE_BLOCKS 64
#define MACHINE_PAGE_SAMPLES (12*MACHINE_PAGE_BLOCKS)
#define MACHINE_PAGES 48
#define MACHINE_RAW_PERIOD 3
#define MACHINE_QUEUE_BLOCKS (MACHINE_PAGES*MACHINE_PAGE_BLOCKS)
#define MACHINE_RAW_BYTES 96
typedef union {
    uint16_t rows[3*MACHINE_PAGE_BLOCKS];
    uint8_t raw[MACHINE_RAW_BYTES];
} machine_page;

// Persistent queue row program, shared by the live transmitter and host tests.
// src is high-pair-first QPSK phase>>1, including six history symbols followed
// by five symbols/block. All four source phases are supported by the wrapper.
// dst holds three uint16_t byte offsets/block, relative to one 16-aligned table.
int machine_build(uint32_t *table, unsigned peak);
void machine_prepare(uint16_t *dst, const uint8_t *src, unsigned symbol, unsigned blocks);
void machine_render(uint32_t *dst, const uint16_t *rows, unsigned blocks, const uint32_t *table);
// Twelve-sample block stream, arbitrary sample slices and destination word alignment.
void machine_samples(uint32_t *dst, const uint16_t *rows, unsigned skip, size_t n,
                     const uint32_t *table);
void machine_reference(uint32_t *dst, const uint8_t *src, unsigned symbol,
                       unsigned blocks, const uint32_t *table);
void machine_prepare_pie(uint16_t *dst, const uint8_t *src, unsigned four_blocks);
void machine_render_pie(uint32_t *dst, const uint16_t *rows, unsigned blocks,
                        const uint32_t *table);
