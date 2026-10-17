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
#include "stream_profile.h"

#define SHAPER_HISTORY 8
#define SHAPER_XOR 0x80200u

typedef struct {
    uint32_t *blocks;      // window w's parts 0, 1, 2 at blocks + w * block_words
    uint16_t n;            // samples per symbol, 4..40
    uint16_t block_words;  // 3 * (n rounded up to 4) rounded up to a power of two
    uint8_t parts;         // 0: three-part table; 2: six-symbol pulse
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
// n40 count contract above. Fast modes use scaled codes from the packed ring.
void shaper_fill40_raw_pie(uint32_t *, const uint8_t *, int, const uint32_t *);
// Any number of whole symbols, including zero. Fast-rate raw phases use the C oracle.
void shaper_fill_raw(uint32_t *dst, const uint8_t *sym, int nsym, const shaper_table_t *t);
// Samples at offset skip within sym[0]. dst must be aligned where a whole symbol starts.
// Partial symbols use aligned scratch; all PIE calls produce whole symbols.
void shaper_samples(uint32_t *dst, const uint8_t *sym, unsigned skip, size_t count,
                    const shaper_table_t *t);
// Same output, optional whole/partial-symbol observations. No per-symbol clock reads.
void shaper_samples_profile(uint32_t *dst, const uint8_t *sym, unsigned skip, size_t count,
                            const shaper_table_t *t, stream_profile_t *whole,
                            stream_profile_t *partial);
// Consumes 16*k for 8PSK / n40, or 16*(k>>1) for QPSK n4/n8 (ring_packed_qpsk).
void shaper_samples_codes_profile(uint32_t *dst, const uint8_t *sym, unsigned skip, size_t count,
                                  const shaper_table_t *t, stream_profile_t *whole,
                                  stream_profile_t *partial);
// Disjoint word buffers, including DAC SRAM. Uses aligned PIE copies where possible;
// scalar edges use word stores, never libc's possible byte stores.
void shaper_copy_words(uint32_t *dst, const uint32_t *src, size_t words);

// Six-symbol 8PSK pulse at 20 Msym/s. sym[-5..nsym] must be readable (one lookahead byte).
void shaper_short_build(shaper_table_t *t, uint32_t *blocks, unsigned peak);
void shaper_short4_pie(uint32_t *dst, const uint8_t *sym, int nsym, const uint32_t *blocks);

// Radix-four specializations of the existing kernels. Same group/output contract.
// Entries consume 16*(phase>>1).
void shaper_fill4_qpsk_pie(uint32_t *, const uint8_t *, int, const uint32_t *);
void shaper_fill8_qpsk_pie(uint32_t *, const uint8_t *, int, const uint32_t *);
