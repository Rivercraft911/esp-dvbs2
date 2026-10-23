#include "lofty.h"
#include "shaper_taps.h"
#include "esp_attr.h"

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
    return 0;
}

IRAM_ATTR uint32_t lofty_prepare(uint32_t *dst, const uint16_t *src, unsigned n, uint32_t previous)
{
    while (n && ((uintptr_t)dst&15)) {
        uint32_t h = previous<<12 | *src++;
        *dst++ = lofty_descriptor(h);
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
        *dst++ = lofty_descriptor(h);
        previous = h&4095;
    }
    return previous;
}

#ifndef __XTENSA__
void lofty_render(uint32_t *dst, const uint32_t *src, unsigned n, const uint32_t *table)
{
    while (n--) {
        uint32_t w = *src++;
        unsigned a = (w>>4)&511, b = (w>>13)&2047, c = (w>>24)&63;
        for (unsigned p = 0; p < 12; ++p) {
            uint32_t av = p < 4 ? table[4*a+p] : table[2048+8*(a&63)+p-4];
            *dst++ = (av+table[2560+4*b+p]+(p < 4 ? 0 :
                table[(p < 8 ? 8704 : 8960)+4*c+(p&3)]))^0x80200;
        }
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

#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
#ifndef LOFTY_CACHE_HOST
#include "esp32s3/rom/cache.h"
#include "esp_cpu.h"
#include "sdkconfig.h"
_Static_assert(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE == 16384 &&
               CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE == 32,
               "lofty residency requires the qualified 16 KiB / 32-byte cache");
_Static_assert(CONFIG_ESP32S3_DATA_CACHE_SIZE >= 32768 &&
               CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE == 32,
               "dummy residency requires at least 32 KiB / 32-byte data cache");
extern const uint8_t _lofty_text_start[], _lofty_text_end[];
#endif
static int resident; // 1 while the control code is locked
static size_t resident_bytes(void)
{
    return ((uintptr_t)_lofty_text_end-(uintptr_t)_lofty_text_start+31)&~31u;
}
static int resident_set(int on)
{
    if (on == resident) return 0;
    const uint32_t first = (uintptr_t)_lofty_text_start;
    const uint32_t bytes = resident_bytes();
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
    if (!r) resident = 1;
    return r;
}
#endif

size_t lofty_resident_bytes(void)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    return resident ? resident_bytes() : 0;
#else
    return 0;
#endif
}
int lofty_resident(int on)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    return resident_set(!!on);
#else
    (void)on;
    return 0;
#endif
}

int lofty_dummy_resident(const uint32_t *dummy, size_t bytes, int on)
{
#if defined(__XTENSA__) || defined(LOFTY_CACHE_HOST)
    const uintptr_t first = (uintptr_t)dummy;
    // An aligned allocation owns every locked line, including the renderer's
    // prefetch guards. Leave over half the 32 KiB data cache for the encoder.
    if (!bytes || (first&31) || (bytes&31) || bytes > 16384 ||
        first < 0x3c000000 || first+bytes > 0x3e000000) return -1;
    if (!on) return Cache_Unlock_Addr(first,bytes);
    const uint32_t before = esp_cpu_get_cycle_count();
    const uint32_t autoload = Cache_Start_DCache_Preload(first,bytes,0);
    while (!Cache_DCache_Preload_Done()) {
        if (esp_cpu_get_cycle_count()-before > 240000) {
            Cache_End_DCache_Preload(autoload);
            return -1;
        }
    }
    Cache_End_DCache_Preload(autoload);
    return Cache_Lock_Addr(first,bytes);
#else
    (void)dummy; (void)bytes; (void)on;
    return 0;
#endif
}
