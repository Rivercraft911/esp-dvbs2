#pragma once
#include <stddef.h>
#include <stdint.h>

// Portable correctness path, not a real-time kernel. sym is 16*phase (0..7),
// with span-1 history symbols. phase = (first_sample*den)%num; num/den is sps.
// Coefficients are Q15 DAC LSB; no allocation, no global state, exactly count stores.
void shaper_rational(uint32_t *dst, const uint8_t *sym, unsigned phase, size_t count,
                     const int32_t *taps, unsigned num, unsigned den, unsigned span);
