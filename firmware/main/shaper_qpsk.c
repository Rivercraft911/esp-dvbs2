// Nine-symbol QPSK pulse, factored by four-symbol input blocks. Checked by tests/test_stream.py.
// Oracle: archive/dev-2026-10-17:host/qpsk_fill.py; docs/how-it-works.md#table-provenance.
#include "shaper_qpsk.h"
#include "shaper_qpsk_taps.h"
#include "shaper_taps.h"
#include "esp_attr.h"

static int value(const int32_t *taps, const int32_t *levels, unsigned n,
                 int part, unsigned w, unsigned p)
{
    int64_t sum = 0;
    for (int j = 0; j < 4; ++j) {
        const int age = (int)(p / n) - (3 - j - 4 * part);
        if (age >= 0 && age < 9)
            sum += (int64_t)levels[2 * ((w >> (2*j)) & 3) + 1] * taps[age*n + p%n];
    }
    return (int)((sum + ((int64_t)1 << (SHAPER_SHIFT-1))) >> SHAPER_SHIFT);
}

int shaper_qpsk_build(uint32_t *table, unsigned n, unsigned level)
{
    if (!table || ((uintptr_t)table & 15) || (n != 3 && n != 4) || level < 16 || level > 508)
        return -1;
    const int32_t *source = n == 3 ? qpsk_taps3 : qpsk_taps4;
    int32_t taps[36];
    for (unsigned i = 0; i < 9*n; ++i)
        taps[i] = (int32_t)((double)source[i] * ((level - 2) / 508.0));
    const int32_t *levels[] = {shaper_level_i, shaper_level_q};
    int bias[2][3][16];
    for (unsigned axis = 0; axis < 2; ++axis) {
        for (unsigned p = 0; p < 4*n; ++p) {
            for (int part = 0; part < 3; ++part) {
                int lo = 0;
                for (unsigned w = 0; w < 256; ++w) {
                    int v = value(taps, levels[axis], n, part, w, p);
                    if (v < lo) lo = v;
                }
                bias[axis][part][p] = -lo;
            }
            bias[axis][1][p] = 512 - bias[axis][0][p] - bias[axis][2][p];
        }
    }
    for (int part = 0; part < 3; ++part)
        for (unsigned w = 0; w < 256; ++w)
            for (unsigned p = 0; p < 4*n; ++p) {
                unsigned i = value(taps, levels[0], n, part, w, p) + bias[0][part][p];
                unsigned q = value(taps, levels[1], n, part, w, p) + bias[1][part][p];
                table[(part*256+w)*4*n+p] = i | q << 10;
            }
    return 0;
}

// Reference only: keep it in flash; the timed probe uses PIE.
void shaper_qpsk_c(uint32_t *dst, const uint8_t *src, unsigned groups,
                            const uint32_t *table, unsigned n)
{
    const unsigned row = 4*n;
    while (groups--) {
        const uint32_t *a = table + row*src[0];
        const uint32_t *b = table + row*(256+src[-1]);
        const uint32_t *c = table + row*(512+src[-2]);
        for (unsigned p = 0; p < row; ++p) *dst++ = (a[p]+b[p]+c[p]) ^ 0x80200;
        ++src;
    }
}

static IRAM_ATTR void fill(uint32_t *dst, const uint8_t *src, unsigned groups,
                           const uint32_t *table, unsigned n)
{
#ifdef __XTENSA__
    if (n == 3) shaper_qpsk3_pie(dst, src, groups, table);
    else shaper_qpsk4_pie(dst, src, groups, table);
#else
    shaper_qpsk_c(dst, src, groups, table, n);
#endif
}

IRAM_ATTR void shaper_qpsk_samples(uint32_t *dst, const uint8_t *src, unsigned skip,
                                  size_t count, const uint32_t *table, unsigned n)
{
    if (!count) return;
    const unsigned row = 4*n;
    uint32_t scratch[16] __attribute__((aligned(16)));
    // The bank schedule gives aligned block boundaries. Keep the general API safe for
    // a caller with a different dst/skip alignment; never let PIE round a store down.
    while (count && (skip || ((uintptr_t)dst & 15) || count < row)) {
        fill(scratch, src++, 1, table, n);
        size_t take = row-skip < count ? row-skip : count;
        for (size_t i = 0; i < take; ++i) *dst++ = scratch[skip+i];
        count -= take;
        skip = 0;
    }
    const size_t groups = count / row;
    if (groups) fill(dst, src, groups, table, n);
    dst += groups*row, src += groups, count -= groups*row;
    if (count) {
        fill(scratch, src, 1, table, n);
        for (size_t i = 0; i < count; ++i) dst[i] = scratch[i];
    }
}
