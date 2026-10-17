#include "shaper_rational.h"

void shaper_rational(uint32_t *dst, const uint8_t *sym, unsigned phase, size_t count,
                     const int32_t *taps, unsigned num, unsigned den, unsigned span)
{
    static const int32_t i[8] = {32768,23170,0,-23170,-32768,-23170,0,23170};
    static const int32_t q[8] = {0,23170,32768,23170,0,-23170,-32768,-23170};
    for (size_t n = 0; n < count; ++n) {
        int64_t si = 0, sq = 0;
        for (unsigned a = 0; a < span; ++a) {
            const unsigned k = sym[-(int)a] >> 4;
            const int32_t c = taps[a*num+phase];
            si += (int64_t)i[k]*c;
            sq += (int64_t)q[k]*c;
        }
        const int32_t vi = (si + (1LL<<29)) >> 30, vq = (sq + (1LL<<29)) >> 30;
        *dst++ = ((uint32_t)vi & 1023) | (((uint32_t)vq & 1023)<<10);
        phase += den;
        sym += phase/num;
        phase %= num;
    }
}
