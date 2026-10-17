// Table addressing on the producer, row sums on the consumer.
#include "machine.h"
#include "esp_attr.h"

// exp/moon-fill 73136ce, same independently designed seven-symbol .20 pulse.
// Units: 1/256 DAC LSB at peak 508; sign levels are Q30. See docs/machine.md.
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
static const unsigned start[] = {0,3,7}, width[] = {3,4,4}, base[] = {0,768,3840};

static int contribution(const int32_t *taps, unsigned part, unsigned code,
                        unsigned p, unsigned axis)
{
    int64_t sum = 0;
    for (unsigned j = 0; j < width[part]; ++j) {
        int at = (int)(5*p) - 12*((int)(start[part]+j)-6);
        unsigned s = (code >> (2*(width[part]-1-j))) & 3;
        int positive = axis ? s < 2 : s == 0 || s == 3;
        if (at >= 0 && at < 84)
            sum += (positive ? INT64_C(759250125) : -INT64_C(759250125))*taps[at];
    }
    return sum < 0 ? -(int)((-sum+(INT64_C(1)<<37))>>38) :
                      (int)((sum+(INT64_C(1)<<37))>>38);
}

int machine_build(uint32_t *table, unsigned peak)
{
    if (!table || ((uintptr_t)table & 15) || peak < 16 || peak > 508) return -1;
    int32_t taps[84];
    int bias[3][12][2] = {0};
    for (unsigned i = 0; i < 84; ++i) taps[i] = (int32_t)((double)pulse[i]*(peak-2)/508.0);
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned c = 0; c < (1u << (2*width[k])); ++c)
            for (unsigned p = 0; p < 12; ++p)
                for (unsigned a = 0; a < 2; ++a) {
                    int v = -contribution(taps,k,c,p,a);
                    if (v > bias[k][p][a]) bias[k][p][a] = v;
                }
    for (unsigned p = 0; p < 12; ++p)
        for (unsigned a = 0; a < 2; ++a)
            bias[0][p][a] += 512-bias[0][p][a]-bias[1][p][a]-bias[2][p][a];
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned c = 0; c < (1u << (2*width[k])); ++c)
            for (unsigned p = 0; p < 12; ++p) {
                unsigned i = contribution(taps,k,c,p,0)+bias[k][p][0];
                unsigned q = contribution(taps,k,c,p,1)+bias[k][p][1];
                table[base[k]+12*c+p] = i | q<<10;
            }
    return 0;
}

static unsigned symbol(const uint8_t *src, unsigned at)
{
    return (src[at/4] >> (6-2*(at&3))) & 3;
}

static void prepare_one(uint16_t *dst, const uint8_t *src, unsigned at)
{
    for (unsigned k = 0; k < 3; ++k) {
        unsigned code = 0;
        for (unsigned j = 0; j < width[k]; ++j) code = 4*code+symbol(src,at+start[k]+j);
        dst[k] = 48*code;  // separate bases avoid two producer additions/block
    }
}

IRAM_ATTR void machine_prepare(uint16_t *dst, const uint8_t *src, unsigned at, unsigned n)
{
#ifdef __XTENSA__
    // Advance at most three blocks to the four-symbol boundary. Four subsequent
    // blocks consume exactly five bytes, so there is no per-frame phase reset.
    while (n && (at&3)) {
        prepare_one(dst,src,at);
        dst += 3, at += 5, --n;
    }
    unsigned runs = n/4;
    if (runs) machine_prepare_pie(dst,src+at/4,runs);
    dst += 12*runs, at += 20*runs, n -= 4*runs;
#endif
    while (n--) {
        prepare_one(dst,src,at);
        dst += 3, at += 5;
    }
}

IRAM_ATTR void machine_render(uint32_t *dst, const uint16_t *rows, unsigned n, const uint32_t *table)
{
#ifdef __XTENSA__
    machine_render_pie(dst,rows,n,table);
#else
    while (n--) {
        for (unsigned p = 0; p < 12; ++p)
            dst[p] = (table[base[0]+rows[0]/4+p]+table[base[1]+rows[1]/4+p]+
                      table[base[2]+rows[2]/4+p]) ^ 0x80200;
        dst += 12, rows += 3;
    }
#endif
}

IRAM_ATTR void machine_samples(uint32_t *dst, const uint16_t *rows, unsigned skip, size_t n,
                              const uint32_t *table)
{
    uint32_t tmp[12] __attribute__((aligned(16)));
    while (n && (skip || ((uintptr_t)dst&15) || n < 12)) {
        machine_render(tmp,rows,1,table);
        size_t take = n < 12-skip ? n : 12-skip;
        for (size_t i = 0; i < take; ++i) *dst++ = tmp[skip+i];
        rows += 3, n -= take, skip = 0;
    }
    unsigned blocks = n/12;
    if (blocks) machine_render(dst,rows,blocks,table);
    dst += 12*blocks, rows += 3*blocks, n -= 12*blocks;
    if (n) {
        machine_render(tmp,rows,1,table);
        for (size_t i = 0; i < n; ++i) dst[i] = tmp[i];
    }
}

void machine_reference(uint32_t *dst, const uint8_t *src, unsigned at,
                       unsigned n, const uint32_t *table)
{
    while (n--) {
        uint16_t row[3];
        prepare_one(row,src,at);
        for (unsigned p = 0; p < 12; ++p) {
            uint32_t v = 0;
            for (unsigned k = 0; k < 3; ++k) v += table[base[k]+row[k]/4+p];
            dst[p] = v ^ 0x80200;
        }
        dst += 12, at += 5;
    }
}
