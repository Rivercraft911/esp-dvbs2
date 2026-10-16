// Isolated APSK prototypes. Not connected to dvb. Model: host/shaper_apsk.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Caller owns 16-byte aligned tables. n=4/8, m=16/32, frac=0/3.
// taps: 9*n signed Q16 DAC counts. levels: m+4 complex points in signed Q14.
// Codes 0..m-1 are standard binary labels; m..m+3 are unit-energy QPSK.
// A single table uses (m+4)*16*n words (padding makes addressing one shift).
// A pair table uses m*m*4*n words plus (m+4)*n tail words.
void apsk_build_single(uint32_t *table, const int32_t *taps,
                       const int16_t (*levels)[2], int m, int n, int frac);
void apsk_build_pair(uint32_t *table, uint32_t *tail, const int32_t *taps,
                     const int16_t (*levels)[2], int m, int n, int frac);
// sym points to the first output symbol, with eight valid history bytes before it.
// Pair: all history/output codes must be < m. Single also accepts QPSK codes.
void apsk_single_c(uint32_t *dst, const uint8_t *sym, size_t count,
                   const uint32_t *table, int n, int frac);
void apsk_pair_c(uint32_t *dst, const uint8_t *sym, size_t count,
                 const uint32_t *table, const uint32_t *tail, int m, int n, int frac);

// PIE: aligned dst/tables, positive groups or zero; eight history bytes required.
// Single emits 9*groups symbols. Pair emits 7*groups data-only symbols.
// No tail overread/overwrite. Wrappers can use C for a partial group.
#define APSK_SINGLE(N, F) \
    void apsk_single_n##N##_f##F(uint32_t *, const uint8_t *, int, const uint32_t *)
APSK_SINGLE(4, 0); APSK_SINGLE(8, 0); APSK_SINGLE(4, 3); APSK_SINGLE(8, 3);
#define APSK_PAIR(M, N, F) \
    void apsk_pair_m##M##_n##N##_f##F(uint32_t *, const uint8_t *, int, const uint32_t *, const uint32_t *)
APSK_PAIR(16, 4, 0); APSK_PAIR(16, 8, 0); APSK_PAIR(16, 4, 3); APSK_PAIR(16, 8, 3);
APSK_PAIR(32, 4, 0); APSK_PAIR(32, 8, 0); APSK_PAIR(32, 4, 3); APSK_PAIR(32, 8, 3);
#undef APSK_SINGLE
#undef APSK_PAIR
