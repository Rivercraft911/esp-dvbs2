// Table build and portable kernel for table-driven pulse shaping (shaper.h, host/shaper.py).

#include "shaper.h"

#include <string.h>

#include "esp_attr.h"
#include "shaper_taps.h"

#define PARTS 3
#define SPAN 3      // symbols per window
#define MAX_N 40

static int block_words(int n)
{
    int w = 1;
    while (w < PARTS * ((n + 3) / 4 * 4)) {
        w <<= 1;
    }
    return w;
}

uint32_t shaper_store[SHAPER_STORE_WORDS] __attribute__((aligned(16)));

IRAM_ATTR void shaper_copy_words(uint32_t *dst, const uint32_t *src, size_t words)
{
    volatile uint32_t *out = dst;
#ifdef __XTENSA__
    void shaper_copy_pie(volatile uint32_t *, const uint32_t *, unsigned);
    while (words && (((uintptr_t)out | (uintptr_t)src) & 15)) {
        *out++ = *src++;
        --words;
    }
    const size_t pairs = words / 8;
    if (pairs) {
        shaper_copy_pie(out, src, (unsigned)pairs);
        out += pairs * 8, src += pairs * 8, words -= pairs * 8;
    }
#endif
    while (words--) *out++ = *src++;
}

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
    const int windows = 1 << (SPAN * bits), bw = block_words(n), row = (n + 3) / 4 * 4;
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
        for (int i = 0; i < bw; ++i) {
            b[i] = 0;
        }
        for (int j = 0; j < PARTS; ++j) {
            for (int p = 0; p < n; ++p) {
                const int fi = part_value(taps, level[0], n, bits, j, w, p) + bias[0][j][p];
                const int fq = part_value(taps, level[1], n, bits, j, w, p) + bias[1][j][p];
                b[j * row + p] = (uint32_t)fi << SHAPER_I_SHIFT | (uint32_t)fq << SHAPER_Q_SHIFT;
            }
        }
    }
}

static IRAM_ATTR void fill_c(uint32_t *dst, const uint8_t *sym, int nsym,
                             const shaper_table_t *t, int shift)
{
    const int n = t->n, row = (n + 3) / 4 * 4, bw = t->block_words;
    const int b = t->bits;
    const uint32_t wmask = (1u << (SPAN * b)) - 1;
    uint32_t h = 0;  // codes, newest in the low bits
    for (int i = -SHAPER_HISTORY; i < 0; ++i) {
        h = h << b | sym[i] >> shift;
    }
    for (int s = 0; s < nsym; ++s) {
        h = h << b | sym[s] >> shift;
        const uint32_t *p0 = t->blocks + (h & wmask) * bw;
        const uint32_t *p1 = t->blocks + (h >> (SPAN * b) & wmask) * bw + row;
        const uint32_t *p2 = t->blocks + (h >> (2 * SPAN * b) & wmask) * bw + 2 * row;
        if (n & 3) {  // 5 samples/symbol, rows padded to eight words
            for (int i = 0; i < n; ++i) {
                *dst++ = (p0[i] + p1[i] + p2[i]) ^ SHAPER_XOR;
            }
            continue;
        }
        for (int i = 0; i < n; i += 4) {
            dst[0] = (p0[0] + p1[0] + p2[0]) ^ SHAPER_XOR;
            dst[1] = (p0[1] + p1[1] + p2[1]) ^ SHAPER_XOR;
            dst[2] = (p0[2] + p1[2] + p2[2]) ^ SHAPER_XOR;
            dst[3] = (p0[3] + p1[3] + p2[3]) ^ SHAPER_XOR;
            dst += 4, p0 += 4, p1 += 4, p2 += 4;
        }
    }
}

IRAM_ATTR void shaper_fill_c(uint32_t *dst, const uint8_t *sym, int nsym, const shaper_table_t *t)
{
    fill_c(dst, sym, nsym, t, t->bits == 3 ? 4 : 5);
}

// At most five tail symbols. Keep the legacy C kernel as an independent oracle.
static IRAM_ATTR void raw_c(uint32_t *dst, const uint8_t *sym, int nsym,
                            const shaper_table_t *t)
{
    uint8_t shifted[SHAPER_HISTORY + 5];
    for (int i = -SHAPER_HISTORY; i < nsym; ++i) {
        shifted[SHAPER_HISTORY + i] = sym[i] << 4;
    }
    shaper_fill_c(dst, shifted + SHAPER_HISTORY, nsym, t);
}

IRAM_ATTR void shaper_fill_raw(uint32_t *dst, const uint8_t *sym, int nsym,
                              const shaper_table_t *t)
{
    if (!nsym) {
        return;
    }
#ifdef __XTENSA__
    if (t->n == 40) {
        shaper_fill40_raw_pie(dst, sym, nsym, t->blocks);
        return;
    }
    const int groups = (nsym - 1) / 6, bulk = 6 * groups + 1;
    (t->bits == 2
        ? (t->n == 8 ? shaper_fill8_qpsk_raw_pie : shaper_fill4_qpsk_raw_pie)
        : (t->n == 8 ? shaper_fill8_raw_pie : shaper_fill4_raw_pie))(dst, sym, groups, t->blocks);
    raw_c(dst + bulk * t->n, sym + bulk, nsym - bulk, t);
#else
    // Host execution of the same edge glue, without Xtensa instructions.
    for (int i = 0; i < nsym; ++i) {
        raw_c(dst + i * t->n, sym + i, 1, t);
    }
#endif
}

IRAM_ATTR void shaper_samples(uint32_t *dst, const uint8_t *sym, unsigned skip, size_t count,
                             const shaper_table_t *t)
{
    shaper_samples_profile(dst, sym, skip, count, t, NULL, NULL);
}

static IRAM_ATTR void fill_codes(uint32_t *dst, const uint8_t *sym, int nsym,
                                 const shaper_table_t *t)
{
#ifdef __XTENSA__
    if (!nsym) return;
    if (t->bits == 2 && t->n != 40) {
        // Same 4/8-sps kernels with radix four, consuming 16*(phase>>1).
        // This branch is also the hook for the separate QPSK 3-sps kernel.
        const int groups = (nsym - 1) / 6, bulk = 6 * groups + 1;
        void (*kernel)(uint32_t *, const uint8_t *, int, const uint32_t *) =
            t->n == 8 ? shaper_fill8_qpsk_pie : shaper_fill4_qpsk_pie;
        kernel(dst, sym, groups, t->blocks);
        for (int i = bulk; i < nsym; ++i)
            kernel(dst + i * t->n, sym + i, 0, t->blocks);
    } else if (t->n == 4) {
        shaper_fill4_all_pie(dst, sym, nsym, t->blocks);
    } else if (t->n == 8) {
        const int groups = (nsym - 1) / 6, bulk = 6 * groups + 1;
        shaper_fill8_pie(dst, sym, groups, t->blocks);
        shaper_fill_c(dst + bulk * 8, sym + bulk, nsym - bulk, t);
    } else {
        shaper_fill40_pie(dst, sym, nsym, t->blocks);
    }
#else
    fill_c(dst, sym, nsym, t, t->n == 40 ? 5 : 4);
#endif
}

static IRAM_ATTR void samples(uint32_t *dst, const uint8_t *sym, unsigned skip,
                              size_t count, const shaper_table_t *t,
                              stream_profile_t *whole, stream_profile_t *partial, int codes)
{
    if (!count) {
        return;
    }
    uint32_t part[MAX_N] __attribute__((aligned(16)));
    const unsigned n = t->n;
    if (skip) {
        const stream_mark_t before = partial ? stream_mark_for(partial) : (stream_mark_t){0};
        (codes ? fill_codes : shaper_fill_raw)(part, sym++, 1, t);
        const size_t take = n - skip < count ? n - skip : count;
        memcpy(dst, part + skip, take * 4);
        dst += take, count -= take;
        if (partial) stream_record(partial, before, stream_mark_for(partial));
    }
    const size_t symbols = count / n;
    if (symbols) {
        const stream_mark_t before = whole ? stream_mark_for(whole) : (stream_mark_t){0};
        (codes ? fill_codes : shaper_fill_raw)(dst, sym, (int)symbols, t);
        if (whole) stream_record(whole, before, stream_mark_for(whole));
    }
    dst += symbols * n, count -= symbols * n, sym += symbols;
    if (count) {
        const stream_mark_t before = partial ? stream_mark_for(partial) : (stream_mark_t){0};
        (codes ? fill_codes : shaper_fill_raw)(part, sym, 1, t);
        memcpy(dst, part, count * 4);
        if (partial) stream_record(partial, before, stream_mark_for(partial));
    }
}

IRAM_ATTR void shaper_samples_profile(uint32_t *dst, const uint8_t *sym, unsigned skip,
                                     size_t count, const shaper_table_t *t,
                                     stream_profile_t *whole, stream_profile_t *partial)
{
    samples(dst, sym, skip, count, t, whole, partial, 0);
}

IRAM_ATTR void shaper_samples_codes_profile(uint32_t *dst, const uint8_t *sym, unsigned skip,
                                           size_t count, const shaper_table_t *t,
                                           stream_profile_t *whole, stream_profile_t *partial)
{
    samples(dst, sym, skip, count, t, whole, partial, 1);
}

// Stage row addresses on the producer core. The consumer needs rows[-6..nsym], one pointer
// per three-symbol window. Supply legacy codes 16*k, sym[-8..nsym). No per-sample work here.
IRAM_ATTR void shaper_rows4(const uint32_t **rows, const uint8_t *sym, int nsym,
                            const uint32_t *blocks)
{
    uint32_t h = (uint32_t)sym[-8] * 8 + sym[-7];
    for (int i = -6; i < nsym; ++i) {
        h = (h * 8 + sym[i]) & 0x1ff0;
        rows[i] = blocks + h;   // 16 * window words = 64 bytes
    }
    rows[nsym] = blocks;  // one harmless look-ahead for the pipelined consumer
}

#ifdef __XTENSA__
// Input is already in internal RAM with its history. A bank wrap may split this into two
// calls; n4 makes both pieces symbol aligned. Avoid the slow C tail and partial-sample path.
IRAM_ATTR void shaper_fill4_all_pie(uint32_t *dst, const uint8_t *sym, int nsym,
                                  const uint32_t *blocks)
{
    if (!nsym) {
        return;
    }
    const int groups = (nsym - 1) / 6, bulk = 6 * groups + 1;
    shaper_fill4_pie(dst, sym, groups, blocks);
    for (int i = bulk; i < nsym; ++i) {
        shaper_fill4_pie(dst + 4 * i, sym + i, 0, blocks);
    }
}

IRAM_ATTR void shaper_fill4_rows_all_pie(uint32_t *dst, const uint32_t *const *rows, int nsym)
{
    if (!nsym) {
        return;
    }
    const int groups = (nsym - 1) / 6, bulk = 6 * groups + 1;
    shaper_fill4_rows_pie(dst, rows, groups);
    for (int i = bulk; i < nsym; ++i) {
        shaper_fill4_rows_pie(dst + 4 * i, rows + i, 0);
    }
}
#endif
