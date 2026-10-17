// Same source for native and QEMU correctness; optional small instruction trace.
#include "machine.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t table[MACHINE_TABLE_WORDS] __attribute__((aligned(16)));
static uint32_t want[16404] __attribute__((aligned(16)));
static uint32_t got[16420] __attribute__((aligned(16)));
static uint32_t rows[1376] __attribute__((aligned(16)));
static uint8_t src[1720];
static uint32_t lut[MACHINE_LUT_WORDS] __attribute__((aligned(16)));

static uint32_t hash(const void *data, unsigned bytes)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    while (bytes--) h = (h ^ *p++)*16777619u;
    return h;
}

static void source(uint32_t x)
{
    for (unsigned i = 0; i < sizeof src; ++i) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        src[i] = x >> 16;
    }
}

static void check(void)
{
    unsigned cases = 0, prepares = 0;
    machine_lut(lut);
    const unsigned peaks[] = {16,128,508}, lengths[] = {0,1,11,12,13,47,48,49,15740,16380};
    const unsigned blocks[] = {0,1,2,3,4,5,7,16,31,1364};
    for (unsigned l = 0; l < 3; ++l) {
        assert(!machine_build(table,peaks[l]));
        for (unsigned seed = 0; seed < 3; ++seed) {
            source((uint32_t[]){2026,7,UINT32_MAX}[seed]);
            for (unsigned phase = 0; phase < 4; ++phase) {
                // Test actual assembly source and destination alignments, all tails.
                for (unsigned a = 0; a < 8; ++a)
                    for (unsigned c = 0; c < sizeof blocks/sizeof *blocks; ++c) {
                        unsigned n = blocks[c];
                        memset(rows,0xa5,sizeof rows);
                        machine_prepare(rows+a,src,phase,n,lut);
                        for (unsigned b = 0; b < n; ++b) {
                            uint32_t value = 0;
                            for (unsigned j = 0; j < 11; ++j) {
                                unsigned at = phase+5*b+j;
                                unsigned c = (src[at/4]>>(6-2*(at&3)))&3;
                                value |= ((c^(c>>1))&1) << (14-j);
                                value |= (c>>1) << (30-j);
                            }
                            assert(rows[a+b] == value);
                        }
                        for (unsigned i = 0; i < a; ++i) assert(rows[i] == 0xa5a5a5a5);
                        for (unsigned i = a+n; i < sizeof rows/sizeof *rows; ++i) assert(rows[i] == 0xa5a5a5a5);
                        ++prepares;
                    }
                machine_prepare(rows,src,phase,1367,lut);
                rows[1367] = 0;
                machine_reference(want,src,phase,1367,table);
                for (unsigned skip = 0; skip < 12; ++skip)
                    for (unsigned a = 0; a < 4; ++a)
                        for (unsigned c = 0; c < sizeof lengths/sizeof *lengths; ++c) {
                            unsigned n = lengths[c], first = 4+a;
                            memset(got,0xa5,sizeof got);
                            machine_samples(got+first,rows,skip,n,table);
                            assert(!memcmp(got+first,want+skip,4*n));
                            for (unsigned i = 0; i < first; ++i) assert(got[i] == 0xa5a5a5a5);
                            for (unsigned i = first+n; i < sizeof got/sizeof *got; ++i) assert(got[i] == 0xa5a5a5a5);
                            ++cases;
                        }
                printf("@{\"test\":\"machine\",\"peak\":%u,\"seed\":%u,\"phase\":%u,"
                       "\"table\":%"PRIu32",\"rows\":%"PRIu32",\"check\":%"PRIu32"}\n",
                       peaks[l],seed,phase,hash(table,sizeof table),hash(rows,4*1367),hash(want,48*1367));
            }
        }
    }
    printf("@{\"test\":\"machine_end\",\"prepares\":%u,\"slices\":%u,\"errors\":0}\n",prepares,cases);
}

#ifdef MACHINE_HOST
int main(int argc, char **argv)
{
    if (argc == 1) { check(); return 0; }
    if (argc != 4) return 2;
    FILE *in = fopen(argv[1],"rb"), *out = fopen(argv[2],"wb");
    unsigned symbols = (unsigned)strtoul(argv[3],NULL,10);
    if (!in || !out || symbols < 11) return 2;
    size_t bytes = (symbols+3)/4;
    uint8_t *input = malloc(bytes);
    if (!input || fread(input,1,bytes,in) != bytes) return 2;
    assert(!machine_build(table,128));
    machine_lut(lut);
    unsigned at = 0, left = (symbols-6)/5;
    while (left) {
        unsigned n = left < 1364 ? left : 1364;
        machine_prepare(rows,input,at,n,lut);
        rows[n] = 0;
        machine_render(got,rows,n,table);
        if (fwrite(got,48,n,out) != n) return 2;
        at += 5*n, left -= n;
    }
    free(input);
    return fclose(in) || fclose(out);
}
#else
void app_main(void)
{
    puts("QEMU-START");
#ifdef MACHINE_TRACE
    source(2026);
    assert(!machine_build(table,128));
    machine_lut(lut);
    for (unsigned n = 4; n <= 1364; n = n == 4 ? 8 : 1364) {
        machine_prepare_pie(rows,src,n/4,lut);
        rows[n] = 0;
        machine_render_pie(got,rows,n,table);
        machine_reference(want,src,0,n,table);
        assert(!memcmp(got,want,48*n));
        if (n == 1364) break;
    }
    puts("machine trace: 4, 8, 1364 blocks, all words agree");
#else
    check();
#endif
    puts("QEMU-END");
    fflush(stdout);
}
#endif
