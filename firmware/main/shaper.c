// Table build and portable kernel for table-driven pulse shaping (shaper.h, host/shaper.py).

#include "shaper.h"

#include "esp_attr.h"
#include "shaper_taps.h"

#define PARTS 3
#define SPAN 3      // symbols per window
#define MAX_N 40

static int block_words(int n)
{
    int w = 1;
    while (w < PARTS * n) {
        w <<= 1;
    }
    return w;
}

uint32_t shaper_store[SHAPER_STORE_WORDS] __attribute__((aligned(16)));

size_t shaper_table_words(int n, int bits)
{
    return ((size_t)1 << (SPAN * bits)) * block_words(n);
}

// Part j of window w at sample p, axis level[]: the three newest-first codes of w at ages
// 3j..3j+2, summed exactly and rounded once to LSB.
static int part_value(const int32_t *taps, const int32_t *level, int n, int bits, int j, int w,
                      int p)
{
    int64_t s = 0;
    for (int a = 0; a < SPAN; ++a) {
        const int code = (w >> (bits * a)) & ((1 << bits) - 1);
        const int k = bits == 3 ? code : 2 * code + 1;
        s += (int64_t)level[k] * taps[(SPAN * j + a) * n + p];
    }
    return (int)((s + ((int64_t)1 << (SHAPER_SHIFT - 1))) >> SHAPER_SHIFT);
}

// Biases: parts 1 and 2 start at 0, part 0 takes the rest of 512, so fields stay in 0..1023 and
// the offset-binary sum of the three parts never carries from I into Q (host/shaper.py checks
// the ranges for the generated taps).
void shaper_build(shaper_table_t *t, uint32_t *blocks, const int32_t *taps, int n, int bits)
{
    const int windows = 1 << (SPAN * bits), bw = block_words(n);
    const int32_t *level[2] = {shaper_level_i, shaper_level_q};
    int bias[2][PARTS][MAX_N];
    *t = (shaper_table_t){.blocks = blocks, .n = n, .block_words = bw, .bits = bits};

    for (int axis = 0; axis < 2; ++axis) {
        for (int p = 0; p < n; ++p) {
            for (int j = 0; j < PARTS; ++j) {
                int lo = 0;
                for (int w = 0; w < windows; ++w) {
                    const int v = part_value(taps, level[axis], n, bits, j, w, p);
                    lo = (w == 0 || v < lo) ? v : lo;
                }
                bias[axis][j][p] = -lo;
            }
            bias[axis][0][p] = 512 - bias[axis][1][p] - bias[axis][2][p];
        }
    }
    for (int w = 0; w < windows; ++w) {
        uint32_t *b = blocks + w * bw;
        for (int j = 0; j < PARTS; ++j) {
            for (int p = 0; p < n; ++p) {
                const int fi = part_value(taps, level[0], n, bits, j, w, p) + bias[0][j][p];
                const int fq = part_value(taps, level[1], n, bits, j, w, p) + bias[1][j][p];
                b[j * n + p] = (uint32_t)fi << SHAPER_I_SHIFT | (uint32_t)fq << SHAPER_Q_SHIFT;
            }
        }
        for (int i = PARTS * n; i < bw; ++i) {
            b[i] = 0;
        }
    }
}

IRAM_ATTR void shaper_fill_c(uint32_t *dst, const uint8_t *sym, int nsym, const shaper_table_t *t)
{
    const int n = t->n, bw = t->block_words, b = t->bits, shift = b == 3 ? 4 : 5;
    const uint32_t wmask = (1u << (SPAN * b)) - 1;
    uint32_t h = 0;  // codes, newest in the low bits
    for (int i = -SHAPER_HISTORY; i < 0; ++i) {
        h = h << b | sym[i] >> shift;
    }
    for (int s = 0; s < nsym; ++s) {
        h = h << b | sym[s] >> shift;
        const uint32_t *p0 = t->blocks + (h & wmask) * bw;
        const uint32_t *p1 = t->blocks + (h >> (SPAN * b) & wmask) * bw + n;
        const uint32_t *p2 = t->blocks + (h >> (2 * SPAN * b) & wmask) * bw + 2 * n;
        for (int i = 0; i < n; i += 4) {  // n is a multiple of 4
            dst[0] = (p0[0] + p1[0] + p2[0]) ^ SHAPER_XOR;
            dst[1] = (p0[1] + p1[1] + p2[1]) ^ SHAPER_XOR;
            dst[2] = (p0[2] + p1[2] + p2[2]) ^ SHAPER_XOR;
            dst[3] = (p0[3] + p1[3] + p2[3]) ^ SHAPER_XOR;
            dst += 4, p0 += 4, p1 += 4, p2 += 4;
        }
    }
}
