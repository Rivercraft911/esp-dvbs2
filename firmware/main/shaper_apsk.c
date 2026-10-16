// Independent table builder and portable reference. No allocation or radio calls.
#include <limits.h>
#include <string.h>
#include "esp_attr.h"
#include "shaper_apsk.h"

static int32_t round_part(int64_t v, int frac)
{
    const int shift = 30 - frac;     // Q14 levels * Q16 taps -> Qfrac DAC counts
    v += INT64_C(1) << (shift - 1);
    return v >= 0 ? (int32_t)(v >> shift) : -(int32_t)((-v + (INT64_C(1) << shift) - 1) >> shift);
}

static int32_t part(const int32_t *taps, const int16_t (*levels)[2],
                    int m, int n, int frac, int pair, int j, int code, int p, int axis)
{
    int age = pair ? 2*j : j;
    if (pair && j == 4) age = 8;
    int first = pair && j < 4 ? code % m : code;
    int64_t v = (int64_t)levels[first][axis] * taps[age*n+p];
    if (pair && j < 4) v += (int64_t)levels[code/m][axis] * taps[(age+1)*n+p];
    return round_part(v, frac);
}

static void build(uint32_t *table, uint32_t *tail, const int32_t *taps,
                  const int16_t (*levels)[2], int m, int n, int frac, int pair)
{
    int bias[9][8][2];
    int groups = pair ? 5 : 9, field = frac ? 16 : 10;
    for (int j = 0; j < groups; ++j) {
        int rows = pair && j < 4 ? m*m : m+4;
        for (int p = 0; p < n; ++p) {
            for (int a = 0; a < 2; ++a) {
                int lo = INT_MAX;
                for (int k = 0; k < rows; ++k) {
                    int v = part(taps, levels, m, n, frac, pair, j, k, p, a);
                    if (v < lo) lo = v;
                }
                bias[j][p][a] = -lo;
            }
        }
    }
    for (int p = 0; p < n; ++p) {
        for (int a = 0; a < 2; ++a) {
            int total = 0;
            for (int j = 0; j < groups; ++j) total += bias[j][p][a];
            // Include the final half-LSB here so packing rounds rather than truncates.
            bias[0][p][a] += (512 << frac) + (frac ? 4 : 0) - total;
        }
    }
    if (!pair) memset(table, 0, (size_t)(m+4)*16*n*sizeof(*table));
    for (int j = 0; j < groups; ++j) {
        int rows = pair && j < 4 ? m*m : m+4;
        for (int k = 0; k < rows; ++k) {
            uint32_t *row = pair && j == 4 ? tail+k*n : table+k*(pair ? 4 : 16)*n+j*n;
            for (int p = 0; p < n; ++p) {
                uint32_t i = part(taps, levels, m, n, frac, pair, j, k, p, 0)+bias[j][p][0];
                uint32_t q = part(taps, levels, m, n, frac, pair, j, k, p, 1)+bias[j][p][1];
                row[p] = i | q << field;
            }
        }
    }
}

void apsk_build_single(uint32_t *table, const int32_t *taps,
                       const int16_t (*levels)[2], int m, int n, int frac)
{
    build(table, NULL, taps, levels, m, n, frac, 0);
}

void apsk_build_pair(uint32_t *table, uint32_t *tail, const int32_t *taps,
                     const int16_t (*levels)[2], int m, int n, int frac)
{
    build(table, tail, taps, levels, m, n, frac, 1);
}

static inline uint32_t pack(uint32_t w, int frac)
{
    if (frac) w = ((w >> 3) & 1023u) | ((w >> 9) & 0xffc00u);
    return w ^ 0x80200u;
}

void IRAM_ATTR apsk_single_c(uint32_t *dst, const uint8_t *sym, size_t count,
                            const uint32_t *table, int n, int frac)
{
    for (size_t k = 0; k < count; ++k, ++sym) {
        for (int p = 0; p < n; ++p) {
            uint32_t w = 0;
            for (int a = 0; a < 9; ++a) w += table[(size_t)sym[-a]*16*n+a*n+p];
            *dst++ = pack(w, frac);
        }
    }
}

void IRAM_ATTR apsk_pair_c(uint32_t *dst, const uint8_t *sym, size_t count,
                          const uint32_t *table, const uint32_t *tail, int m, int n, int frac)
{
    for (size_t k = 0; k < count; ++k, ++sym) {
        for (int p = 0; p < n; ++p) {
            uint32_t w = tail[sym[-8]*n+p];
            for (int a = 0; a < 4; ++a) {
                int code = sym[-2*a]+m*sym[-2*a-1];
                w += table[(size_t)code*4*n+a*n+p];
            }
            *dst++ = pack(w, frac);
        }
    }
}
