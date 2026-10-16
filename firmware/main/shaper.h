#pragma once

// Table-driven pulse shaping into DAC words: signed 10-bit I in bits 9:0, Q in bits 19:10.
// host/shaper.py is the model and explains the method: a 9-symbol pulse split into three parts
// of three symbols, each window's three part rows stored in one block, three rows added per
// symbol and the sum XORed with 0x80200.
//
// Symbols are bytes 16 * k for the phase k * pi / 4, k = 0..7 (QPSK uses k = 1, 3, 5, 7). Every
// kernel reads SHAPER_HISTORY symbols before sym[0], so keep them in front of the first symbol.

#include <stddef.h>
#include <stdint.h>

#define SHAPER_HISTORY 8
#define SHAPER_XOR 0x80200u

typedef struct {
    uint32_t *blocks;      // window w's parts 0, 1, 2 at blocks + w * block_words
    uint16_t n;            // samples per symbol
    uint16_t block_words;  // 3 * n rounded up to a power of two
    uint8_t bits;          // bits per symbol code: 3 (eight phases) or 2 (QPSK)
} shaper_table_t;

// Words a table needs: 2^(3 bits) windows of block_words each.
size_t shaper_table_words(int n, int bits);

// Static table storage in internal RAM, large enough for any table here (8PSK at 8 samples/symbol
// is the largest, 16384 words). Static because once the DAC banks are reserved the largest free
// internal heap block is exactly 64 KiB, too small for a 64 KiB aligned allocation.
#define SHAPER_STORE_WORDS 16384
extern uint32_t shaper_store[SHAPER_STORE_WORDS];

// Build a table from integer taps (shaper_taps.h) into blocks (16-byte aligned,
// shaper_table_words() words).
void shaper_build(shaper_table_t *t, uint32_t *blocks, const int32_t *taps, int n, int bits);

// Portable kernel: nsym symbols -> nsym * t->n words.
void shaper_fill_c(uint32_t *dst, const uint8_t *sym, int nsym, const shaper_table_t *t);

// PIE kernels (shaper_pie.S), dst 16-byte aligned, tables from shaper_build().
// 8 samples/symbol, 3-bit codes: 6 * groups + 1 symbols -> (6 * groups + 1) * 8 words.
void shaper_fill8_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks);
// 40 samples/symbol, 2-bit codes (QPSK): nsym >= 1 symbols -> nsym * 40 words.
void shaper_fill40_pie(uint32_t *dst, const uint8_t *sym, int nsym, const uint32_t *blocks);
