#include "lofty.h"
#include "shaper_taps.h"
#include "esp_attr.h"

#if LOFTY_FUSED
// The two fragments must be rounded independently, even after table fusion.
static int lofty_term(unsigned w, unsigned count, int position, unsigned p,
                       unsigned axis, const int32_t *taps)
{
    int64_t sum = 0;
    for (unsigned j = 0; j < count; ++j) {
        int h = (int)p-3*(position+(int)j);
        unsigned phase = (w>>(3*(count-1-j)))&7;
        if (h >= 0 && h < 15)
            sum += (int64_t)(axis ? shaper_level_q[phase] : shaper_level_i[phase])*taps[h];
    }
    int value = (int)(((sum < 0 ? -sum : sum)+(INT64_C(1)<<37))>>38);
    return sum < 0 ? -value : value;
}
#endif

int lofty_build(uint32_t *table, unsigned peak)
{
    // tests/reference/psk8_26m667.py: inverse-hold RRC, alpha .35, five symbols, Kaiser beta 1.5.
    static const int32_t pulse[15] = {
        3186,4025,-4203,-15205,-8356,28954,78499,101737,
        78499,28954,-8356,-15205,-4203,4025,3186
    };
    if (!table || ((uintptr_t)table&15) || peak < 16 || peak > 508) return -1;
    int32_t taps[15];
    for (unsigned i = 0; i < 15; ++i) taps[i] = (int32_t)((double)pulse[i]*(peak-2)/508.0);
    int bias[3][2][12] = {0};
    // Preserve the original three independent roundings and offset biases.
    for (unsigned part = 0; part < 3; ++part) {
        unsigned count = part == 2 ? 2 : 3;
        for (unsigned w = 0; w < (1u<<(3*count)); ++w)
            for (unsigned axis = 0; axis < 2; ++axis)
                for (unsigned p = 0; p < 12; ++p) {
                    int64_t sum = 0;
                    for (unsigned j = 0; j < count; ++j) {
                        int h = (int)p-3*(-4+(int)(3*part+j));
                        unsigned phase = w>>(3*(count-1-j))&7;
                        if (h >= 0 && h < 15)
                            sum += (int64_t)(axis ? shaper_level_q[phase] : shaper_level_i[phase])*taps[h];
                    }
                    int v = (int)(((sum < 0 ? -sum : sum)+(INT64_C(1)<<37))>>38);
                    if (v > bias[part][axis][p]) bias[part][axis][p] = v;
                }
    }
    for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned p = 0; p < 12; ++p)
            bias[0][axis][p] = 512-bias[1][axis][p]-bias[2][axis][p];
#if LOFTY_FUSED
    // Chronological symbols a..h become a,b,c,g,h,d,e,f in the queue.
    // A0 = abc; (A1+C1) = bcg; (A2+C2) = cgh; B0..2 = def.
    for (unsigned plane = 0; plane < 6; ++plane)
        for (unsigned w = 0; w < 512; ++w)
            for (unsigned j = 0; j < 4; ++j) {
                unsigned p = 4*(plane < 3 ? plane : plane-3)+j;
                uint32_t packed = 0;
                for (unsigned axis = 0; axis < 2; ++axis) {
                    int value;
                    if (plane == 0)
                        value = lofty_term(w,3,-4,p,axis,taps)+bias[0][axis][p];
                    else if (plane == 1)
                        value = lofty_term(w>>3,2,-3,p,axis,taps)+
                                lofty_term(w&7,1,2,p,axis,taps)+bias[0][axis][p]+bias[2][axis][p];
                    else if (plane == 2)
                        value = lofty_term(w>>6,1,-2,p,axis,taps)+
                                lofty_term(w&63,2,2,p,axis,taps)+bias[0][axis][p]+bias[2][axis][p];
                    else value = lofty_term(w,3,-1,p,axis,taps)+bias[1][axis][p];
                    packed |= (uint32_t)value<<(10*axis);
                }
                table[2048*plane+4*w+j] = packed;
            }
#else
    // A's first vector needs three symbols, its last two need only two.
    // Keep those two vectors adjacent to save a renderer address calculation.
    const unsigned parts[] = {0,0,1,2,2}, first[] = {0,4,0,4,8};
    const unsigned counts[] = {3,2,3,2,2}, widths[] = {4,8,12,4,4};
    for (unsigned v = 0; v < 5; ++v) {
        unsigned part = parts[v], count = counts[v], width = widths[v];
        unsigned rows = 1u<<(3*count);
        for (unsigned w = 0; w < rows; ++w)
            for (unsigned p = 0; p < width; ++p) {
                uint32_t packed = 0;
                for (unsigned axis = 0; axis < 2; ++axis) {
                    int64_t sum = 0;
                    for (unsigned j = 0; j < count; ++j) {
                        int position = part == 0 ? -1-(int)count+(int)j : -4+(int)(3*part+j);
                        int h = (int)(first[v]+p)-3*position;
                        unsigned phase = w>>(3*(count-1-j))&7;
                        if (h >= 0 && h < 15)
                            sum += (int64_t)(axis ? shaper_level_q[phase] : shaper_level_i[phase])*taps[h];
                    }
                    int value = (int)(((sum < 0 ? -sum : sum)+(INT64_C(1)<<37))>>38);
                    if (sum < 0) value = -value;
                    packed |= (uint32_t)(value+bias[part][axis][first[v]+p])<<(10*axis);
                }
                table[width*w+p] = packed;
            }
        table += width*rows;
    }
#endif
    return 0;
}

uint32_t lofty_prepare(uint32_t *dst, const uint16_t *src, unsigned n, uint32_t previous)
{
    while (n && ((uintptr_t)dst&15)) {
        uint32_t h = previous<<12 | *src++;
        *dst++ = (LOFTY_FUSED ? lofty_fused_descriptor(h) : lofty_scaled_descriptor(h));
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
        *dst++ = (LOFTY_FUSED ? lofty_fused_descriptor(h) : lofty_scaled_descriptor(h));
        previous = h&4095;
    }
    return previous;
}

#ifndef __XTENSA__
void lofty_render(uint32_t *dst, const uint32_t *src, unsigned n, const uint32_t *table)
{
    while (n--) {
        uint32_t w = *src++;
#if LOFTY_FUSED
        unsigned indices[] = {(w>>19)&511,(w>>16)&511,(w>>13)&511};
        unsigned b = (w>>4)&511;
        for (unsigned p = 0; p < 12; ++p)
            *dst++ = (table[2048*(p/4)+4*indices[p/4]+(p&3)]+
                      table[2048*(3+p/4)+4*b+(p&3)])^0x80200;
#else
        unsigned a = (w>>4)&511, b = (w>>13)&2047, c = (w>>24)&63;
        for (unsigned p = 0; p < 12; ++p) {
            uint32_t av = p < 4 ? table[4*a+p] : table[2048+8*(a&63)+p-4];
            *dst++ = (av+table[2560+4*b+p]+(p < 4 ? 0 :
                table[(p < 8 ? 8704 : 8960)+4*c+(p&3)]))^0x80200;
        }
#endif
    }
}

void lofty_render_pages(uint32_t *dst, const uint32_t *src, unsigned pages,
                        const uint32_t *table, uint32_t *consumed)
{
    while (pages--) {
        lofty_render(dst,src,LOFTY_BLOCKS,table);
        dst += 12*LOFTY_BLOCKS; src += LOFTY_BLOCKS+4;
        __atomic_store_n(consumed,*consumed+1,__ATOMIC_RELEASE);
    }
}
#endif

LOFTY_TEXT void lofty_samples(uint32_t *dst, const uint32_t *src, unsigned skip, size_t n,
                             const uint32_t *table)
{
    uint32_t tmp[12] __attribute__((aligned(16)));
    while (n) {
        if (!skip && !((uintptr_t)dst&15) && n >= 12) {
            unsigned blocks = n/12;
            lofty_render(dst,src,blocks,table);
            dst += 12*blocks; src += blocks; n -= 12*blocks;
        } else {
            lofty_render(tmp,src++,1,table);
            size_t take = n < 12-skip ? n : 12-skip;
            for (size_t i = 0; i < take; ++i) *dst++ = tmp[skip+i];
            n -= take; skip = 0;
        }
    }
}

#if !LOFTY_FUSED
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
#ifndef LOFTY_CACHE_HOST
#include "esp32s3/rom/cache.h"
#include "esp_cpu.h"
#include "sdkconfig.h"
_Static_assert(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE == 16384 &&
               CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE == 32,
               "lofty residency requires the qualified 16 KiB / 32-byte cache");
extern const uint8_t _lofty_text_start[], _lofty_text_end[];
extern const uint8_t _lofty_fused_text_start[], _lofty_fused_text_end[];
#endif
static int resident; // 0: unlocked, 1: default region, 2: fused region
static size_t resident_bytes(unsigned mode)
{
    const uintptr_t first = mode == 1 ? (uintptr_t)_lofty_text_start : (uintptr_t)_lofty_fused_text_start;
    const uintptr_t end = mode == 1 ? (uintptr_t)_lofty_text_end : (uintptr_t)_lofty_fused_text_end;
    return (end-first+31)&~31u;
}
static int resident_set(int on, int mode)
{
    if (on ? resident == mode : resident != mode) return 0;
    if (on && resident) return -1; // never lock both regions
    const uint32_t first = mode == 1 ? (uintptr_t)_lofty_text_start : (uintptr_t)_lofty_fused_text_start;
    const uint32_t bytes = resident_bytes(mode);
    if (!on) {
        int r = Cache_Unlock_Addr(first,bytes);
        if (!r) resident = 0;
        return r;
    }
    const uint32_t before = esp_cpu_get_cycle_count();
    const uint32_t autoload = Cache_Start_ICache_Preload(first,bytes,0);
    while (!Cache_ICache_Preload_Done()) {
        if (esp_cpu_get_cycle_count()-before > 240000) {
            Cache_End_ICache_Preload(autoload);
            return -1;
        }
    }
    Cache_End_ICache_Preload(autoload);
    int r = Cache_Lock_Addr(first,bytes);
    if (!r) resident = mode;
    return r;
}
#endif

size_t lofty_resident_bytes(void)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    return resident == 1 ? resident_bytes(1) : 0;
#else
    return 0;
#endif
}
size_t lofty_fused_resident_bytes(void)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    return resident == 2 ? resident_bytes(2) : 0;
#else
    return 0;
#endif
}
int lofty_resident(int on)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    return resident_set(on,1);
#else
    (void)on;
    return 0;
#endif
}
int lofty_fused_resident(int on)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    return resident_set(on,2);
#else
    (void)on;
    return 0;
#endif
}
#endif // !LOFTY_FUSED
