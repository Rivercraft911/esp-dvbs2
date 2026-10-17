// Peripheral-free C/PIE experiment. Full banks and guarded block counts.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaper_moon.h"

static uint32_t table[24672] __attribute__((aligned(16)));
static uint32_t expected[16392] __attribute__((aligned(16)));
static uint32_t guarded[16400] __attribute__((aligned(16)));
static uint16_t input[1368] __attribute__((aligned(16)));

static uint32_t hash(const uint32_t *p, unsigned words)
{
    uint32_t h = 2166136261u;
    while (words--) h = (h ^ *p++)*16777619u;
    return h;
}

int moon_check(void)
{
    const unsigned counts[] = {0,1,2,3,4,5,7,31,1365};
    const unsigned peaks[] = {16,128,508};
    const uint32_t seeds[] = {2026,7,UINT32_MAX};
    unsigned bad = 0, slices = 0, slice_bad = 0;
    for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned level = 0; level < 3; ++level) {
            const unsigned peak = peaks[level], words = shaper_moon_words(mode);
            if (shaper_moon_build(table,mode,peak)) return 1;
            for (unsigned seed = 0; seed < 3; ++seed) {
                uint32_t x = seeds[seed];
                for (unsigned i = 0; i < sizeof input/sizeof input[0]; ++i) {
                    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                    input[i] = x & ((1u << (mode == 0 ? 12 : mode == 1 ? 10 : 15))-1);
                }
                for (unsigned c = 0; c < sizeof counts/sizeof counts[0]; ++c) {
                    const unsigned n = counts[c];
                    memset(guarded,0xa5,sizeof guarded);
                    shaper_moon_c(expected,input+2,n,table,mode);
                    shaper_moon_fill(guarded+4,input+2,n,table,mode);
                    unsigned mismatch = 0, guards = 1;
                    for (unsigned j = 0; j < 12*n; ++j) mismatch += expected[j] != guarded[j+4];
                    for (unsigned j = 0; j < 4; ++j)
                        guards &= guarded[j] == 0xa5a5a5a5 && guarded[4+12*n+j] == 0xa5a5a5a5;
                    bad |= mismatch || !guards;
                    printf("@{\"test\":\"moon_fill\",\"mode\":%u,\"peak\":%u,\"seed\":%"PRIu32
                           ",\"groups\":%u,\"errors\":%u,\"guards\":%u,\"table\":%"PRIu32
                           ",\"check\":%"PRIu32"}\n",mode,peak,seeds[seed],n,mismatch,guards,
                           hash(table,words),hash(guarded+4,12*n));
                }
                shaper_moon_c(expected,input+2,1366,table,mode);
                const unsigned lengths[] = {0,1,2,3,4,11,12,13,16380};
                for (unsigned skip = 0; skip < 12; ++skip)
                    for (unsigned offset = 0; offset < 4; ++offset)
                        for (unsigned c = 0; c < sizeof lengths/sizeof lengths[0]; ++c) {
                            const unsigned n = lengths[c], first = 4+offset;
                            memset(guarded,0xa5,sizeof guarded);
                            shaper_moon_samples(guarded+first,input+2,skip,n,table,mode);
                            for (unsigned j = 0; j < n; ++j)
                                slice_bad |= guarded[first+j] != expected[skip+j];
                            for (unsigned j = 0; j < first; ++j) slice_bad |= guarded[j] != 0xa5a5a5a5;
                            for (unsigned j = first+n; j < sizeof guarded/sizeof guarded[0]; ++j)
                                slice_bad |= guarded[j] != 0xa5a5a5a5;
                            ++slices;
                        }
            }
        }
    printf("@{\"test\":\"moon_slices\",\"cases\":%u,\"errors\":%u}\n",slices,slice_bad);
    return bad || slice_bad;
}

#ifdef MOON_HOST
int main(int argc, char **argv)
{
    if (argc == 1) return moon_check();
    if (argc != 4) return 2;
    const unsigned mode = (unsigned)strtoul(argv[1],NULL,10);
    if (mode >= 3) return 2;
    FILE *in = fopen(argv[2],"rb"), *out = fopen(argv[3],"wb");
    if (!in || !out) return 2;
    if (fseek(in,0,SEEK_END)) return 2;
    const long bytes = ftell(in);
    if (bytes < 4 || bytes%2 || fseek(in,0,SEEK_SET)) return 2;
    uint16_t *src = malloc((size_t)bytes);
    if (!src || fread(src,1,(size_t)bytes,in) != (size_t)bytes) return 2;
    if (shaper_moon_build(table,mode,128)) return 2;
    size_t at = 2, remaining = (size_t)bytes/2-2;
    while (remaining) {
        const unsigned take = remaining < 1365 ? (unsigned)remaining : 1365;
        shaper_moon_fill(expected,src+at,take,table,mode);
        if (fwrite(expected,4,12*take,out) != 12*take) return 2;
        at += take, remaining -= take;
    }
    free(src);
    return fclose(in) || fclose(out);
}
#else
void app_main(void)
{
    puts("QEMU-START");
    if (moon_check()) puts("MOON-FAIL");
    puts("QEMU-END");
}
#endif
