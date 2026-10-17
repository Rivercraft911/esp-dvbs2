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
    uint16_t n;            // samples per symbol, 4..40
    uint16_t block_words;  // 3 * (n rounded up to 4) rounded up to a power of two
    uint8_t bits;          // bits per symbol code: 3 (eight phases) or 2 (QPSK)
} shaper_table_t;

// Words a table needs: 2^(3 bits) windows of block_words each.
size_t shaper_table_words(int n, int bits);

// Static table storage in internal RAM, large enough for any table here (8PSK at 8 samples/symbol
// is the largest, 16384 words). Static storage avoids a contiguous 64 KiB heap allocation
// after the DAC banks are reserved. An 8PSK table at 4 samples/symbol uses the first 8192 words.
#define SHAPER_STORE_WORDS 16384
extern uint32_t shaper_store[SHAPER_STORE_WORDS];

// Build a table from integer taps (shaper_taps.h) into blocks (16-byte aligned,
// shaper_table_words() words).
void shaper_build(shaper_table_t *t, uint32_t *blocks, const int32_t *taps, int n, int bits);

// Portable kernel: nsym >= 0 symbols -> nsym * t->n words, 4 <= n <= 40.
void shaper_fill_c(uint32_t *dst, const uint8_t *sym, int nsym, const shaper_table_t *t);

// PIE kernels (shaper_pie.S), dst 16-byte aligned, tables from shaper_build().
// 4 samples/symbol, 3-bit codes: groups >= 0, 6 * groups + 1 symbols -> (6 * groups + 1) * 4
// words. For other lengths use C for the last 0..5 symbols. Each 64-byte window block holds
// three 16-byte part rows followed by 16 bytes of padding; every PIE load is aligned.
void shaper_fill4_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks);
// Any nsym >= 0, same history/alignment contract. PIE also handles the 0..5-symbol tail.
// Intended for a fill reading immutable, prestaged internal symbols, not a PSRAM callback.
void shaper_fill4_all_pie(uint32_t *dst, const uint8_t *sym, int nsym, const uint32_t *blocks);
// 8 samples/symbol, 3-bit codes: 6 * groups + 1 symbols -> (6 * groups + 1) * 8 words.
void shaper_fill8_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks);
// 40 samples/symbol, 2-bit codes (QPSK): nsym >= 1 symbols -> nsym * 40 words.
void shaper_fill40_pie(uint32_t *dst, const uint8_t *sym, int nsym, const uint32_t *blocks);

// Encoder phases k = 0..7, with eight raw history bytes. The raw PIE variants have the
// same group/count contracts as above. No shifted copy of a PLFRAME is needed.
void shaper_fill4_raw_pie(uint32_t *, const uint8_t *, int, const uint32_t *);
void shaper_fill8_raw_pie(uint32_t *, const uint8_t *, int, const uint32_t *);
void shaper_fill40_raw_pie(uint32_t *, const uint8_t *, int, const uint32_t *);
// Any number of whole symbols, including zero. C handles the last 0..5 at 4/8 sps.
void shaper_fill_raw(uint32_t *dst, const uint8_t *sym, int nsym, const shaper_table_t *t);
// Samples at offset skip within sym[0]. dst must be aligned where a whole symbol starts.
// Partial symbols use aligned scratch; all PIE calls produce whole symbols.
void shaper_samples(uint32_t *dst, const uint8_t *sym, unsigned skip, size_t count,
                    const shaper_table_t *t);

// The prototypes below take legacy codes 16*k, like the non-raw kernels above.
// Producer-side row staging, codes sym[-8..nsym), rows[-6..nsym] (one valid guard).
// Keep rows internal and immutable until the consumer finishes. This helper does not provide
// thread synchronization or a queue. n4 only; native pointers are four bytes on the S3.
void shaper_rows4(const uint32_t **rows, const uint8_t *sym, int nsym, const uint32_t *blocks);
// n4 with staged row addresses, same 6*groups+1 geometry as shaper_fill4_pie.
void shaper_fill4_rows_pie(uint32_t *dst, const uint32_t *const *rows, int groups);
void shaper_fill4_rows_all_pie(uint32_t *dst, const uint32_t *const *rows, int nsym);
// n5, 3-bit codes, padded eight-word part rows. Four symbols/group, groups >= 0;
// exactly 20*groups words, aligned dst, no writes outside output. Eight history codes.
void shaper_fill5_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks);
