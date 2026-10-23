// Full-axis sign tables (source f704aa0), rendered from compact I/Q bit planes.
#include "machine.h"
#include "esp_attr.h"

// Seven-symbol pulse (source 73136ce). 1/256 DAC LSB at peak 508.
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

#ifndef __XTENSA__
// Host oracles for the tests: whole eleven-sign windows rendered one block at a time.
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

void machine_render(uint32_t *dst, const uint32_t *signs, unsigned n, const uint32_t *table)
{
    const unsigned shift[] = {3,2,0};
    while (n--) {
        uint32_t w = *signs++;
        for (unsigned v = 0; v < 3; ++v) {
            const uint32_t *i = table+(v*256+((w>>(shift[v]+4))&255))*4;
            const uint32_t *q = table+((v+3)*256+((w>>(shift[v]+20))&255))*4;
            for (unsigned p = 0; p < 4; ++p) *dst++ = i[p] | q[p];
        }
    }
}

uint32_t machine_expand(uint32_t *dst, const uint32_t *axes, uint32_t history)
{
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
#endif

// One 192-sample chunk. The Xtensa kernel extracts the three windows per axis
// directly from the two rolling 32-bit sign streams; no history page is stored.
static inline __attribute__((always_inline)) void axis_render(uint32_t *dst, const uint32_t *axes, unsigned chunk,
                               uint32_t previous, const uint32_t *table, unsigned runs)
{
    const unsigned at = 5*chunk;
    if (at) previous = axes[at-1];
#ifdef __XTENSA__
    machine_axis_render_pie(dst,axes+at,runs,previous,table);
#else
    uint32_t history = (previous << 4) & 0x7ff07ff0;
    unsigned pending = 0;
    for (unsigned k = 0; k < 5*runs; ++k) {
        uint32_t w = axes[at+k];
        for (int shift = 15; shift >= 0; --shift) {
            uint32_t signs = w >> shift;
            history = ((history << 1) & 0x7ff07ff0) |
                      ((signs & 1) << 4) | ((signs & 0x10000) << 4);
            if (++pending == 5) {
                uint32_t pair[2] = {history,0};
                machine_render(dst,pair,1,table);
                dst += 12;
                pending = 0;
            }
        }
    }
#endif
}

static inline __attribute__((always_inline)) void axis_copy(uint32_t *dst, const uint32_t *src, size_t n)
{
    volatile uint32_t *out = dst;
#ifdef __XTENSA__
    void shaper_copy_pie(volatile uint32_t *, const uint32_t *, unsigned);
    while (n && (((uintptr_t)out | (uintptr_t)src)&15)) {
        *out++ = *src++;
        --n;
    }
    unsigned pairs = n/8;
    if (pairs) shaper_copy_pie(out,src,pairs);
    out += 8*pairs, src += 8*pairs, n -= 8*pairs;
#endif
    while (n--) *out++ = *src++;
}

IRAM_ATTR void machine_axis_samples(uint32_t *dst, const uint32_t *axes, uint32_t previous,
                                    unsigned skip, size_t n, const uint32_t *table, uint32_t *scratch)
{
    while (n) {
        const unsigned chunk = skip/MACHINE_AXIS_CHUNK_SAMPLES;
        const unsigned cut = skip%MACHINE_AXIS_CHUNK_SAMPLES;
        if (!cut && !((uintptr_t)dst&15) && n >= MACHINE_AXIS_CHUNK_SAMPLES) {
            unsigned runs = n/MACHINE_AXIS_CHUNK_SAMPLES;
            axis_render(dst,axes,chunk,previous,table,runs);
            unsigned done = MACHINE_AXIS_CHUNK_SAMPLES*runs;
            dst += done, n -= done, skip += done;
        } else {
            axis_render(scratch,axes,chunk,previous,table,1);
            size_t take = MACHINE_AXIS_CHUNK_SAMPLES-cut;
            if (take > n) take = n;
            axis_copy(dst,scratch+cut,take);
            dst += take, n -= take, skip += take;
        }
    }
}
