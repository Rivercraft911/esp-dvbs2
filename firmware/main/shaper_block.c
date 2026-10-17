// Four-symbol factorisation of exp/rd-short's six-symbol, inverse-hold pulse.
// Regrouping changes rounding, so this is deliberately separate from legacy tables.
#include "shaper_block.h"
#include "shaper_taps.h"

// exp/rd-short 1698304, shaper_taps_8psk_n3_a35_s6. host/maxrate.py derives these.
static const int32_t pulse[18] = {
    -1684, 1817, 5606, 1249, -11208, -15587, 6665, 51998, 90010,
    90010, 51998, 6665, -15587, -11208, 1249, 5606, 1817, -1684
};

static int value(const int32_t *taps, const int32_t *levels, int part, int w, int p)
{
    int64_t sum = 0;
    for (int k = 0; k < 3; ++k) {
        const int position = 3 * part - 3 - k;
        const int age = p / 3 - position;
        if (age >= 0 && age < 6)
            sum += (int64_t)levels[(w >> (3 * k)) & 7] * taps[age * 3 + p % 3];
    }
    const int64_t half = (int64_t)1 << (SHAPER_SHIFT - 1);
    return sum < 0 ? -(int)((-sum + half) >> SHAPER_SHIFT) :
                     (int)((sum + half) >> SHAPER_SHIFT);
}

void shaper_block_build(uint32_t *tables[3])
{
    int32_t taps[18];
    int maximum = 0;
    for (int p = 0; p < 3; ++p) {
        int sum = 0;
        for (int a = 0; a < 6; ++a) {
            int v = pulse[a * 3 + p];
            sum += v < 0 ? -v : v;
        }
        if (sum > maximum) maximum = sum;
    }
    // Peak 128 LSB, with 3 LSB reserved for three complex roundings.
    const double scale = 125.0 / (maximum / 256.0);
    for (int i = 0; i < 18; ++i) taps[i] = (int32_t)(pulse[i] * scale);
    int bias[2][3][12];
    const int32_t *levels[] = {shaper_level_i, shaper_level_q};
    for (int axis = 0; axis < 2; ++axis) {
        for (int p = 0; p < 12; ++p) {
            for (int part = 0; part < 3; ++part) {
                int lo = 0;
                for (int w = 0; w < 512; ++w) {
                    int v = value(taps, levels[axis], part, w, p);
                    if (v < lo) lo = v;
                }
                bias[axis][part][p] = -lo;
            }
            bias[axis][1][p] = 512 - bias[axis][0][p] - bias[axis][2][p];
        }
    }
    for (int part = 0; part < 3; ++part)
        for (int w = 0; w < 512; ++w)
            for (int p = 0; p < 12; ++p) {
                unsigned i = value(taps, levels[0], part, w, p) + bias[0][part][p];
                unsigned q = value(taps, levels[1], part, w, p) + bias[1][part][p];
                tables[part][12 * w + p] = i | q << 10;
            }
}

void shaper_block_c(uint32_t *dst, const uint16_t *src, unsigned groups,
                    const uint32_t *t0, const uint32_t *t1, const uint32_t *t2)
{
    if (!groups) return;
    uint32_t h = (uint32_t)src[-2] << 12 | src[-1];
    for (unsigned i = 0; i < groups; ++i) {
        h = h << 12 | src[i];
        const uint32_t *a = t0 + ((h >> 18) & 511) * 12;
        const uint32_t *b = t1 + ((h >> 9) & 511) * 12;
        const uint32_t *c = t2 + (h & 511) * 12;
        for (int p = 0; p < 12; ++p) *dst++ = (a[p] + b[p] + c[p]) ^ 0x80200;
    }
}
