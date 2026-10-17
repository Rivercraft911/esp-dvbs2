// Experimental block tables. Independent convolution and rational pulse: host/moon_fill.py.
#include "shaper_moon.h"
#include "shaper_moon_taps.h"
#include "shaper_taps.h"
#include "esp_attr.h"

static const struct {
    uint8_t up, den, bits, parts, span, first[4], count[4], shift[4], row[4];
    const int32_t *pulse;
} modes[] = {
    {3,1,3,3,18,{0,3,6},{3,3,3},{18,9,0},{12,12,12},moon_8psk3},
    {12,5,2,3,84,{0,1,6},{1,5,5},{20,10,0},{4,12,12},moon_qpsk24},
    {12,5,3,4,84,{0,3,6,9},{3,3,3,2},{24,15,6,0},{12,12,12,12},moon_8psk24},
    {12,5,2,3,84,{0,3,7},{3,4,4},{16,8,0},{12,12,12},moon_qpsk24}
};

unsigned shaper_moon_parts(unsigned mode)
{
    return mode < SHAPER_MOON_MODES ? modes[mode].parts : 0;
}

size_t shaper_moon_part_words(unsigned mode, unsigned part)
{
    if (mode >= SHAPER_MOON_MODES || part >= modes[mode].parts) return 0;
    return (size_t)modes[mode].row[part] << (modes[mode].bits*modes[mode].count[part]);
}

size_t shaper_moon_words(unsigned mode)
{
    if (mode >= SHAPER_MOON_MODES) return 0;
    size_t words = 0;
    for (unsigned j = 0; j < modes[mode].parts; ++j)
        words += shaper_moon_part_words(mode,j);
    return words;
}

static int contribution(unsigned mode, const int32_t *taps, unsigned axis,
                         unsigned part, unsigned word, unsigned p)
{
    const int32_t *levels = axis ? shaper_level_q : shaper_level_i;
    const unsigned bits = modes[mode].bits, count = modes[mode].count[part];
    const int origin = mode ? -6 : -5;
    int64_t sum = 0;
    for (unsigned j = 0; j < count; ++j) {
        const int position = origin+modes[mode].first[part]+j;
        const int at = (int)(p*modes[mode].den)-position*modes[mode].up;
        unsigned phase = word >> (bits*(count-1-j)) & ((1u << bits)-1);
        if (bits == 2) phase = 2*phase+1;
        if (at >= 0 && at < modes[mode].span) sum += (int64_t)levels[phase]*taps[at];
    }
    // Odd rounding preserves all quarter-turn identities, including zero.
    return sum < 0 ? -(int)((-sum+((int64_t)1 << 37)) >> 38) :
                      (int)((sum+((int64_t)1 << 37)) >> 38);
}

int shaper_moon_build(uint32_t *table, unsigned mode, unsigned peak)
{
    if (!table || ((uintptr_t)table & 15) || mode >= SHAPER_MOON_MODES || peak < 16 || peak > 508) return -1;
    int32_t taps[84];
    const unsigned parts = modes[mode].parts, bits = modes[mode].bits;
    for (unsigned i = 0; i < modes[mode].span; ++i)
        taps[i] = (int32_t)((double)modes[mode].pulse[i]*(peak-(parts+1)/2)/508.0);
    int bias[4][2][12] = {0};
    for (unsigned part = 0; part < parts; ++part)
        for (unsigned axis = 0; axis < 2; ++axis)
            for (unsigned w = 0; w < (1u << (bits*modes[mode].count[part])); ++w)
                for (unsigned p = 0; p < 12; ++p) {
                    int v = contribution(mode,taps,axis,part,w,p);
                    if (-v > bias[part][axis][p]) bias[part][axis][p] = -v;
                }
    for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned p = 0; p < 12; ++p) {
            int total = 0;
            for (unsigned part = 0; part < parts; ++part) total += bias[part][axis][p];
            bias[mode == 1 ? parts-1 : 0][axis][p] += 512-total;
        }
    for (unsigned part = 0; part < parts; ++part)
        for (unsigned w = 0; w < (1u << (bits*modes[mode].count[part])); ++w)
            for (unsigned p = 0; p < modes[mode].row[part]; ++p) {
                const unsigned i = contribution(mode,taps,0,part,w,p)+bias[part][0][p];
                const unsigned q = contribution(mode,taps,1,part,w,p)+bias[part][1][p];
                *table++ = i | q << 10;
            }
    return 0;
}

void shaper_moon_c(uint32_t *dst, const uint16_t *src, unsigned groups,
                   const uint32_t *table, unsigned mode)
{
    const unsigned step = mode == 0 ? 12 : mode == 2 ? 15 : 10;
    uint64_t h = (uint64_t)src[-2] << step | src[-1];
    while (groups--) {
        h = h << step | *src++;
        const uint32_t *t = table;
        for (unsigned p = 0; p < 12; ++p) dst[p] = 0;
        for (unsigned j = 0; j < modes[mode].parts; ++j) {
            const unsigned rows = 1u << (modes[mode].bits*modes[mode].count[j]);
            const unsigned w = h >> modes[mode].shift[j] & (rows-1);
            const unsigned row = modes[mode].row[j];
            for (unsigned p = 0; p < row; ++p) dst[p] += t[row*w+p];
            t += row*rows;
        }
        for (unsigned p = 0; p < 12; ++p) dst[p] ^= 0x80200;
        dst += 12;
    }
}

IRAM_ATTR void shaper_moon_fill_tables(uint32_t *dst, const uint16_t *src, unsigned groups,
                                      const uint32_t *const *tables, unsigned mode)
{
#ifdef __XTENSA__
    if (mode == 0) shaper_moon3_pie(dst,src,groups,tables[0],tables[1],tables[2]);
    else if (mode == 3) shaper_moon24qc_pie(dst,src,groups,tables[0],tables[1],tables[2]);
    else if (mode == 1) shaper_moon24q_pie(dst,src,groups,tables);
    else shaper_moon24p_pie(dst,src,groups,tables);
#else
    const unsigned step = mode == 0 ? 12 : mode == 2 ? 15 : 10;
    uint64_t h = (uint64_t)src[-2] << step | src[-1];
    while (groups--) {
        h = h << step | *src++;
        for (unsigned p = 0; p < 12; ++p) dst[p] = 0;
        for (unsigned j = 0; j < modes[mode].parts; ++j) {
            const unsigned rows = 1u << (modes[mode].bits*modes[mode].count[j]);
            const unsigned w = h >> modes[mode].shift[j] & (rows-1);
            const unsigned row = modes[mode].row[j];
            for (unsigned p = 0; p < row; ++p) dst[p] += tables[j][row*w+p];
        }
        for (unsigned p = 0; p < 12; ++p) dst[p] ^= 0x80200;
        dst += 12;
    }
#endif
}

IRAM_ATTR void shaper_moon_fill(uint32_t *dst, const uint16_t *src, unsigned groups,
                               const uint32_t *table, unsigned mode)
{
    const uint32_t *tables[4];
    for (unsigned j = 0; j < shaper_moon_parts(mode); ++j) {
        tables[j] = table;
        table += shaper_moon_part_words(mode,j);
    }
    shaper_moon_fill_tables(dst,src,groups,tables,mode);
}

IRAM_ATTR void shaper_moon_sample_tables(uint32_t *dst, const uint16_t *src, unsigned skip,
                                  size_t count, const uint32_t *const *tables, unsigned mode)
{
    if (!count) return;
    uint32_t scratch[12] __attribute__((aligned(16)));
    while (count && (skip || ((uintptr_t)dst & 15) || count < 12)) {
        shaper_moon_fill_tables(scratch,src++,1,tables,mode);
        size_t take = 12-skip < count ? 12-skip : count;
        for (size_t i = 0; i < take; ++i) *dst++ = scratch[skip+i];
        count -= take;
        skip = 0;
    }
    const size_t groups = count/12;
    if (groups) shaper_moon_fill_tables(dst,src,groups,tables,mode);
    dst += 12*groups, src += groups, count -= 12*groups;
    if (count) {
        shaper_moon_fill_tables(scratch,src,1,tables,mode);
        for (size_t i = 0; i < count; ++i) dst[i] = scratch[i];
    }
}

// Canonical 16*phase bytes for 8PSK; high-pair-first quad bytes for QPSK.
// offset is in symbols. The caller includes two whole input blocks of history.
IRAM_ATTR __attribute__((noinline)) void shaper_moon_pack(uint16_t *dst, const uint8_t *src,
                                                        unsigned offset, unsigned groups, unsigned mode)
{
    if (mode == 1 || mode == 3) {
        while (groups && (offset&3)) {
            const uint8_t *p = src+offset/4;
            *dst++ = ((p[0]<<8 | p[1]) >> (6-2*(offset&3))) & 1023;
            offset += 5;
            --groups;
        }
        src += offset/4;
        while (groups >= 4) {
            dst[0] = src[0]<<2 | src[1]>>6;
            dst[1] = (src[1]<<4 | src[2]>>4) & 1023;
            dst[2] = (src[2]<<6 | src[3]>>2) & 1023;
            dst[3] = (src[3]<<8 | src[4]) & 1023;
            src += 5;
            dst += 4;
            groups -= 4;
        }
        offset = 0;
        while (groups--) {
            const uint8_t *p = src+offset/4;
            *dst++ = ((p[0]<<8 | p[1]) >> (6-2*(offset&3))) & 1023;
            offset += 5;
        }
    } else {
        src += offset;
        if (mode == 0) {
            while (groups--) {
                *dst++ = src[0]<<5 | src[1]<<2 | src[2]>>1 | src[3]>>4;
                src += 4;
            }
        } else {
            while (groups--) {
                *dst++ = src[0]<<8 | src[1]<<5 | src[2]<<2 | src[3]>>1 | src[4]>>4;
                src += 5;
            }
        }
    }
}

void shaper_moon_samples(uint32_t *dst, const uint16_t *src, unsigned skip, size_t count,
                         const uint32_t *table, unsigned mode)
{
    const uint32_t *tables[4];
    for (unsigned j = 0; j < shaper_moon_parts(mode); ++j) {
        tables[j] = table;
        table += shaper_moon_part_words(mode,j);
    }
    shaper_moon_sample_tables(dst,src,skip,count,tables,mode);
}

IRAM_ATTR void shaper_moon_quad_samples(uint32_t *dst, const uint8_t *src, unsigned offset,
                                       unsigned skip, size_t count, const uint32_t *const *tables)
{
    uint16_t blocks[6];
    while (count) {
#ifdef __XTENSA__
        if (!skip && !((uintptr_t)dst & 15) && !(offset & 3) && count >= 48) {
            const unsigned runs = count/48;
            shaper_moon24qc_quads_pie(dst,src+offset/4+1,runs,tables);
            dst += 48*runs;
            offset += 20*runs;
            count -= 48*runs;
            continue;
        }
#endif
        // Pack each edge once, including its history. At most four blocks
        // reach the next quad boundary; the rest goes through the fused loop.
        unsigned groups = (skip+count+11)/12;
        if (groups > 4) groups = 4-(offset&3);
        const size_t available = 12*groups-skip;
        const size_t take = available < count ? available : count;
        shaper_moon_pack(blocks,src,offset,groups+2,3);
        shaper_moon_sample_tables(dst,blocks+2,skip,take,tables,3);
        dst += take;
        offset += 5*groups;
        count -= take;
        skip = 0;
    }
}
