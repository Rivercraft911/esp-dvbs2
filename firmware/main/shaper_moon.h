#pragma once
#include <stddef.h>
#include <stdint.h>

// Experiment only, not selected by the transmitter. No allocation and no radio access.
// mode 0: 8PSK 3 sps, 1: QPSK 12/5 sps, 2: 8PSK 12/5 sps.
// Inputs are chronological blocks of 4 (mode 0) or 5 symbols packed high first
// in uint16_t words, 2 bits/symbol for QPSK phase>>1, 3 bits for 8PSK phase.
// Keep two whole words of history before src[0]. Every group emits twelve samples.
size_t shaper_moon_words(unsigned mode);
int shaper_moon_build(uint32_t *table, unsigned mode, unsigned peak);
void shaper_moon_c(uint32_t *dst, const uint16_t *src, unsigned groups,
                   const uint32_t *table, unsigned mode);
// Aligned table and output; positive or zero group count. Tables built above.
void shaper_moon3_pie(uint32_t *, const uint16_t *, unsigned, const uint32_t *,
                      const uint32_t *, const uint32_t *);
void shaper_moon24q_pie(uint32_t *, const uint16_t *, unsigned, const uint32_t *const *);
void shaper_moon24p_pie(uint32_t *, const uint16_t *, unsigned, const uint32_t *const *);
void shaper_moon_fill(uint32_t *dst, const uint16_t *src, unsigned groups,
                      const uint32_t *table, unsigned mode);
// skip is the first sample's phase in the global twelve-sample block. Arbitrary
// dst alignment and length; scalar word stores at edges, aligned vectors inside.
void shaper_moon_samples(uint32_t *dst, const uint16_t *src, unsigned skip, size_t count,
                         const uint32_t *table, unsigned mode);
