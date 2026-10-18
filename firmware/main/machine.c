// Full-axis sign tables from exp/moon-wild-a f704aa0, prepared a page at a time.
#include "machine.h"
#include "esp_attr.h"

// Seven-symbol pulse from exp/moon-fill 73136ce. 1/256 DAC LSB at peak 508.
static const int32_t pulse[84] = {
    -3579,-4265,-4743,-4206,-2604,-3794,-2601,-1020,100,2911,4975,6875,
    10146,10151,9793,10518,8619,8950,6091,1014,-3414,-9060,-13121,-16662,
    -19318,-21083,-20834,-21002,-19046,-11800,-3008,8385,18731,29365,42662,
    57427,69908,82201,93581,99861,107620,107867,107867,107620,99861,93581,
    82201,69908,57427,42662,29365,18731,8385,-3008,-11800,-19046,-21002,
    -20834,-21083,-19318,-16662,-13121,-9060,-3414,1014,6091,8950,8619,
    10518,9793,10151,10146,6875,4975,2911,100,-1020,-2601,-3794,-2604,
    -4206,-4743,-4265,-3579
};

int machine_build(uint32_t *table, unsigned peak)
{
    if (!table || ((uintptr_t)table & 15) || peak < 16 || peak > 508) return -1;
    int32_t taps[84];
    for (unsigned k = 0; k < 84; ++k) taps[k] = (int32_t)((double)pulse[k]*((peak-1)/508.0));
    const int newest[] = {1,2,4};
    for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned v = 0; v < 3; ++v)
            for (unsigned w = 0; w < 256; ++w)
                for (unsigned p = 0; p < 4; ++p) {
                    int64_t sum = 0;
                    for (int b = 0; b < 8; ++b) {
                        int h = 5*(int)(4*v+p)-12*(newest[v]-b);
                        if (h >= 0 && h < 84)
                            sum += ((w >> b & 1) ? -INT64_C(759250125) : INT64_C(759250125))*taps[h];
                    }
                    int value = (int)(((sum < 0 ? -sum : sum)+(INT64_C(1)<<37))>>38);
                    if (sum < 0) value = -value;
                    table[((axis*3+v)*256+w)*4+p] = ((uint32_t)value & 1023) << (10*axis);
                }
    return 0;
}

void machine_lut(uint32_t *lut)
{
    for (unsigned w = 0; w < MACHINE_LUT_WORDS; ++w) {
        uint32_t value = 0;
        for (unsigned j = 0; j < 5; ++j) {
            unsigned c = w >> (2*j);
            value |= ((c ^ (c >> 1)) & 1) << (j+4);
            value |= ((c >> 1) & 1) << (j+20);
        }
        lut[w] = value;
    }
}

static unsigned symbol(const uint8_t *src, unsigned at)
{
    return (src[at/4] >> (6-2*(at&3))) & 3;
}

static uint32_t prepare_one(const uint8_t *src, unsigned at)
{
    uint32_t state = 0;
    for (unsigned j = 0; j < 11; ++j) {
        unsigned c = symbol(src,at+j);
        state = ((state << 1) & 0x7ff07ff0) | ((c ^ (c >> 1)) & 1) << 4 | (c >> 1) << 20;
    }
    return state;
}

IRAM_ATTR void machine_prepare(uint32_t *dst, const uint8_t *src, unsigned at, unsigned n,
                               const uint32_t *lut)
{
#ifdef __XTENSA__
    while (n && (at&3)) {
        *dst++ = prepare_one(src,at);
        at += 5, --n;
    }
    unsigned runs = n/4;
    if (runs) machine_prepare_pie(dst,src+at/4,runs,lut);
    dst += 4*runs, at += 20*runs, n -= 4*runs;
#else
    (void)lut;
#endif
    while (n--) {
        *dst++ = prepare_one(src,at);
        at += 5;
    }
}

IRAM_ATTR void machine_render(uint32_t *dst, const uint32_t *signs, unsigned n, const uint32_t *table)
{
#ifdef __XTENSA__
    machine_render_pie(dst,signs,n,table);
#else
    const unsigned shift[] = {3,2,0};
    while (n--) {
        uint32_t w = *signs++;
        for (unsigned v = 0; v < 3; ++v) {
            const uint32_t *i = table+(v*256+((w>>(shift[v]+4))&255))*4;
            const uint32_t *q = table+((v+3)*256+((w>>(shift[v]+20))&255))*4;
            for (unsigned p = 0; p < 4; ++p) *dst++ = i[p] | q[p];
        }
    }
#endif
}

IRAM_ATTR uint32_t machine_expand(uint32_t *dst, const uint32_t *axes, uint32_t history)
{
#ifdef __XTENSA__
    uint32_t dvbs2_qpsk_histories(uint32_t *, const uint8_t *, unsigned, uint32_t);
    return dvbs2_qpsk_histories(dst,(const uint8_t *)axes,MACHINE_PAGE_BLOCKS/16,history);
#else
    unsigned pending = 0;
    for (unsigned k = 0; k < MACHINE_PAGE_AXIS_WORDS; ++k) {
        uint32_t w = axes[k];
        for (int shift = 15; shift >= 0; --shift) {
            const uint32_t signs = w >> shift;
            history = ((history << 1) & 0x7ff07ff0) |
                      (signs & 1) << 4 | (signs & 0x10000) << 4;
            if (++pending == 5) { *dst++ = history; pending = 0; }
        }
    }
    return history;
#endif
}

IRAM_ATTR void machine_samples(uint32_t *dst, const uint32_t *signs, unsigned skip, size_t n,
                              const uint32_t *table)
{
    uint32_t tmp[12] __attribute__((aligned(16)));
    while (n && (skip || ((uintptr_t)dst&15) || n < 12)) {
        machine_render(tmp,signs,1,table);
        size_t take = n < 12-skip ? n : 12-skip;
        for (size_t i = 0; i < take; ++i) *dst++ = tmp[skip+i];
        ++signs, n -= take, skip = 0;
    }
    unsigned blocks = n/12;
    if (blocks) machine_render(dst,signs,blocks,table);
    dst += 12*blocks, signs += blocks, n -= 12*blocks;
    if (n) {
        machine_render(tmp,signs,1,table);
        for (size_t i = 0; i < n; ++i) dst[i] = tmp[i];
    }
}

void machine_reference(uint32_t *dst, const uint8_t *src, unsigned at,
                       unsigned n, const uint32_t *table)
{
    while (n--) {
        uint32_t signs[2] = {prepare_one(src,at),0};
        machine_render(dst,signs,1,table);
        dst += 12, at += 5;
    }
}
