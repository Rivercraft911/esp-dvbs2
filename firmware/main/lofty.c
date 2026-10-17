#include "lofty.h"
#include "shaper_taps.h"
#include "esp_attr.h"

int lofty_build(uint32_t *table, unsigned peak)
{
    // host/lofty.py: inverse-hold RRC, alpha .35, five symbols, Kaiser beta 1.5.
    static const int32_t pulse[15] = {
        3186,4025,-4203,-15205,-8356,28954,78499,101737,
        78499,28954,-8356,-15205,-4203,4025,3186
    };
    if (!table || ((uintptr_t)table&15) || peak < 16 || peak > 508) return -1;
    int32_t taps[15];
    for (unsigned i = 0; i < 15; ++i) taps[i] = (int32_t)((double)pulse[i]*(peak-2)/508.0);
    int bias[3][2][12] = {0};
    for (unsigned part = 0; part < 3; ++part) {
        const unsigned count = part == 2 ? 2 : 3, rows = 1u<<(3*count);
        for (unsigned axis = 0; axis < 2; ++axis)
            for (unsigned w = 0; w < rows; ++w)
                for (unsigned p = 0; p < 12; ++p) {
                    int64_t sum = 0;
                    for (unsigned j = 0; j < count; ++j) {
                        int h = (int)p-3*(-4+(int)(3*part+j));
                        unsigned phase = w>>(3*(count-1-j))&7;
                        if (h >= 0 && h < 15)
                            sum += (int64_t)(axis ? shaper_level_q[phase] : shaper_level_i[phase])*taps[h];
                    }
                    int value = (int)(((sum < 0 ? -sum : sum)+(INT64_C(1)<<37))>>38);
                    if (sum < 0) value = -value;
                    if (-value > bias[part][axis][p]) bias[part][axis][p] = -value;
                    // First pass stores signed contributions temporarily, axis separately.
                    if (!axis) table[12*w+p] = (uint32_t)value;
                    else table[12*w+p] = (table[12*w+p]&1023) | ((uint32_t)value&1023)<<10;
                }
        table += 12*rows;
    }
    table -= 12*(512+512+64);
    for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned p = 0; p < 12; ++p)
            bias[0][axis][p] += 512-bias[0][axis][p]-bias[1][axis][p]-bias[2][axis][p];
    for (unsigned part = 0; part < 3; ++part) {
        unsigned rows = part == 2 ? 64 : 512;
        for (unsigned w = 0; w < rows; ++w)
            for (unsigned p = 0; p < 12; ++p) {
                uint32_t packed = 0, v = table[12*w+p];
                for (unsigned axis = 0; axis < 2; ++axis) {
                    int value = (int)((v>>(10*axis))&1023);
                    if (value >= 512) value -= 1024;
                    packed |= (uint32_t)(value+bias[part][axis][p])<<(10*axis);
                }
                table[12*w+p] = packed;
            }
        table += 12*rows;
    }
    // Two copies of each newest-pair vector absorb the overlapping index bit.
    // The first four samples have no contribution from this pair.
    uint32_t tail[256];
    table -= 12*64;
    for (unsigned w = 0; w < 64; ++w)
        for (unsigned p = 0; p < 4; ++p) tail[4*w+p] = table[12*w+8+p];
    for (unsigned w = 0; w < 64; ++w)
        for (unsigned p = 0; p < 4; ++p)
            table[8*w+p] = table[8*w+4+p] = table[12*w+4+p];
    for (unsigned w = 0; w < 64; ++w)
        for (unsigned p = 0; p < 4; ++p)
            table[512+8*w+p] = table[512+8*w+4+p] = tail[4*w+p];
    return 0;
}

uint32_t lofty_prepare(uint32_t *dst, const uint16_t *src, unsigned n, uint32_t previous)
{
    while (n && ((uintptr_t)dst&15)) {
        uint32_t h = previous<<12 | *src++;
        *dst++ = (((h>>15)&511)*3<<4) | (((h>>6)&511)*3<<15) | ((h&63)<<26);
        previous = h&4095;
        --n;
    }
#ifdef __XTENSA__
    unsigned vectors = n/8;
    if (vectors) previous = lofty_prepare_pie(dst,src,vectors,previous);
    dst += 8*vectors; src += 8*vectors; n -= 8*vectors;
#endif
    while (n--) {
        uint32_t h = previous<<12 | *src++;
        *dst++ = (((h>>15)&511)*3<<4) | (((h>>6)&511)*3<<15) | ((h&63)<<26);
        previous = h&4095;
    }
    return previous;
}

IRAM_ATTR void lofty_render(uint32_t *dst, const uint32_t *src, unsigned n, const uint32_t *table)
{
#ifdef __XTENSA__
    lofty_render_pie(dst,src,n,table);
#else
    while (n--) {
        uint32_t w = *src++;
        const uint32_t *a = table+((w&32752)/4);
        const uint32_t *b = table+6144+(((w>>11)&32752)/4);
        const uint32_t *c = table+12288+(((w>>21)&2032)/4);
        for (unsigned p = 0; p < 12; ++p)
            *dst++ = (a[p]+b[p]+(p < 4 ? 0 : p < 8 ? c[p-4] : c[512+p-8]))^0x80200;
    }
#endif
}

IRAM_ATTR void lofty_samples(uint32_t *dst, const uint32_t *src, unsigned skip, size_t n,
                             const uint32_t *table)
{
    uint32_t tmp[12] __attribute__((aligned(16)));
    while (n && (skip || ((uintptr_t)dst&15) || n < 12)) {
        lofty_render(tmp,src++,1,table);
        size_t take = n < 12-skip ? n : 12-skip;
        for (size_t i = 0; i < take; ++i) *dst++ = tmp[skip+i];
        n -= take, skip = 0;
    }
    unsigned blocks = n/12;
    if (blocks) lofty_render(dst,src,blocks,table);
    dst += 12*blocks, src += blocks, n -= 12*blocks;
    if (n) {
        lofty_render(tmp,src,1,table);
        for (size_t i = 0; i < n; ++i) dst[i] = tmp[i];
    }
}
