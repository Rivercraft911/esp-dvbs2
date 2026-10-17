#include "lofty.h"
#include "dvbs2.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef LOFTY_TRACE
#define LOFTY_TRACE 0
#endif

static uint32_t table[LOFTY_TABLE_WORDS] __attribute__((aligned(16)));
static uint32_t got[16420] __attribute__((aligned(16)));
static uint32_t want[16420] __attribute__((aligned(16)));
static uint32_t offsets[1380] __attribute__((aligned(16)));
static uint16_t words[1380] __attribute__((aligned(16)));

static uint32_t hash(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    while (n--) h = (h^*p++)*16777619u;
    return h;
}

static void reference(uint32_t *dst, unsigned blocks)
{
    // Independent of the PIE pointer pipeline and wrapper, including history.
    for (unsigned b = 0; b < blocks; ++b) {
        uint32_t h = (uint32_t)words[b]<<12 | words[b+1];
        for (unsigned p = 0; p < 12; ++p)
            *dst++ = (table[12*((h>>15)&511)+p] + table[12*(512+((h>>6)&511))+p] +
                      (p < 4 ? 0 : p < 8 ? table[12288+8*(h&63)+p-4] : table[12800+8*(h&63)+p-8]))^0x80200;
    }
}

static void kernels(void)
{
    unsigned cases = 0;
    for (unsigned peak = 0; peak < 3; ++peak)
        for (unsigned seed = 0; seed < 3; ++seed) {
            assert(!lofty_build(table,(unsigned[]){16,128,508}[peak]));
            uint32_t x = (uint32_t[]){2026,7,UINT32_MAX}[seed];
            for (unsigned i = 0; i < 1380; ++i) {
                x ^= x<<13; x ^= x>>17; x ^= x<<5;
                words[i] = x&4095;
            }
            reference(want,1367);
            for (unsigned source = 0; source < 8; ++source)
              for (unsigned align = 0; align < 4; ++align) {
                memset(offsets,0xa5,sizeof offsets);
                uint32_t old = source ? words[source-1] : 0;
                uint32_t previous = lofty_prepare(offsets+align,words+source,1367,old);
                assert(previous == words[source+1366]);
                for (unsigned j = 0; j < 1367; ++j) {
                    uint32_t h = old<<12 | words[source+j];
                    uint32_t expected = (((h>>15)&511)*3<<4) | (((h>>6)&511)*3<<15) | ((h&63)<<26);
                    assert(offsets[align+j] == expected);
                    old = words[source+j];
                }
                for (unsigned j = 0; j < align; ++j) assert(offsets[j] == 0xa5a5a5a5);
                for (unsigned j = align+1367; j < 1380; ++j) assert(offsets[j] == 0xa5a5a5a5);
            }
            assert(lofty_prepare(offsets,words+1,1367,words[0]) == words[1367]);
            offsets[1367] = 0;
            const unsigned lengths[] = {0,1,11,12,13,47,48,49,15740,16380};
            for (unsigned skip = 0; skip < 12; ++skip)
                for (unsigned align = 0; align < 4; ++align)
                    for (unsigned k = 0; k < sizeof lengths/sizeof *lengths; ++k) {
                        unsigned n = lengths[k], first = 4+align;
                        memset(got,0xa5,sizeof got);
                        lofty_samples(got+first,offsets,skip,n,table);
                        assert(!memcmp(got+first,want+skip,4*n));
                        for (unsigned i = 0; i < first; ++i) assert(got[i] == 0xa5a5a5a5);
                        for (unsigned i = first+n; i < 16420; ++i) assert(got[i] == 0xa5a5a5a5);
                        ++cases;
                    }
            printf("@{\"test\":\"lofty\",\"peak\":%u,\"seed\":%u,\"table\":%"PRIu32",\"samples\":%"PRIu32"}\n",
                   (unsigned[]){16,128,508}[peak],seed,hash(table,sizeof table),hash(want,1367*48));
        }
    printf("@{\"test\":\"lofty_end\",\"slices\":%u,\"errors\":0}\n",cases);
}

// Trace sentinels. The parser uses linked PCs, never QEMU's CCOUNT.
static unsigned saved_ps;
__attribute__((noinline)) void lofty_begin(void)
{
#ifdef __XTENSA__
    __asm__ volatile("rsil %0,15" : "=a"(saved_ps));
#else
    saved_ps = 0;
#endif
}
__attribute__((noinline)) void lofty_end(void)
{
#ifdef __XTENSA__
    __asm__ volatile("wsr.ps %0; rsync" :: "a"(saved_ps));
#else
    (void)saved_ps;
#endif
}

static void encoder(void)
{
    dvbs2_fast *f = aligned_alloc(16,(dvbs2_fast_size()+15)&~(size_t)15);
    dvbs2_workspace *ws = aligned_alloc(16,(sizeof *ws+15)&~(size_t)15);
    uint8_t *payload = aligned_alloc(16,8112), *raw = aligned_alloc(16,33312);
    uint32_t *map = aligned_alloc(16,DVBS2_LOFTY_CACHE_BYTES), *bch = aligned_alloc(16,8192);
    assert(f && ws && payload && raw && map && bch);
    const unsigned sizes[] = {0,1,2,3,7,128,131};
    unsigned cases = 0;
    for (unsigned mc = LOFTY_TRACE ? 18 : 12; mc <= 17; ++mc)
        for (unsigned pilots = 0; pilots < 2; ++pilots) {
            const dvbs2_mode *m = dvbs2_find_mode(mc);
            unsigned n = dvbs2_frame_symbols(m,pilots), bytes = (m->kbch-80)/8;
            dvbs2_sign_state state = {0};
            uint32_t h = 0, x = 2026+mc;
            unsigned pending = 0, call = 0;
            for (unsigned frame = 0; frame < 3; ++frame) {
                for (unsigned j = 0; j < bytes; ++j) { x ^= x<<13; x ^= x>>17; x ^= x<<5; payload[j] = x; }
                assert(!dvbs2_fast_prepare(f,m,pilots,15));
                assert(!dvbs2_encode_frame(f,ws,NULL,payload,8*bytes,raw));
                assert(!dvbs2_fast_lofty_cache(f,map));
                if (m->t > 8) assert(!dvbs2_fast_cache_bch(f,(uint8_t *)bch));
                assert(!dvbs2_fast_lofty_start(f,ws,&state));
                unsigned at = 0;
                while (state.at < n) {
                    uint16_t out[140];
                    for (unsigned j = 0; j < 140; ++j) out[j] = 0xa5a5;
                    unsigned room = sizes[call++%7], got_n = dvbs2_fast_lofty_read(f,&state,out+1,room);
                    assert(got_n <= room);
                    for (unsigned j = 0; j < got_n; ++j) {
                        do { h = ((h<<3)|raw[at++])&4095; } while (++pending < 4);
                        pending = 0;
                        assert(out[1+j] == h);
                    }
                    assert(out[0] == 0xa5a5);
                    for (unsigned j = 1+got_n; j < 140; ++j) assert(out[j] == 0xa5a5);
                }
                while (at < n) { h = ((h<<3)|raw[at++])&4095; ++pending; }
                assert(state.pending == pending && state.history == h);
                ++cases;
            }
        }
    printf("@{\"test\":\"lofty_mapper\",\"frames\":%u,\"errors\":0}\n",cases);
#if LOFTY_TRACE
    for (unsigned mc = 13; mc <= 17; mc += 2) {
        const dvbs2_mode *m = dvbs2_find_mode(mc);
        unsigned bytes = (m->kbch-80)/8;
        uint32_t x = 2026;
        for (unsigned i = 0; i < bytes; ++i) { x ^= x<<13; x ^= x>>17; x ^= x<<5; payload[i] = x; }
        assert(!dvbs2_fast_prepare(f,m,1,15));
        if (m->t > 8) assert(!dvbs2_fast_cache_bch(f,(uint8_t *)bch));
        assert(!dvbs2_fast_lofty_cache(f,map));
        dvbs2_sign_state state = {0};
        lofty_begin(); dvbs2_fast_bb_gcs(f,ws,payload,8*bytes); lofty_end();
        lofty_begin(); dvbs2_fast_bch(f,ws); lofty_end();
        lofty_begin(); dvbs2_fast_ldpc(f,ws); lofty_end();
        lofty_begin(); dvbs2_fast_lofty_start(f,ws,&state); lofty_end();
        lofty_begin();
        uint32_t previous = 0;
        while (state.at < dvbs2_frame_symbols(m,1)) {
            unsigned count = dvbs2_fast_lofty_read(f,&state,words,128);
            previous = lofty_prepare(offsets,words,count,previous);
        }
        lofty_end();
        printf("trace frame %u complete\n",mc);
        printf("@{\"test\":\"lofty_allocation\",\"modcod\":%u,\"fast_bytes\":%u,\"payload_bytes\":%u,\"bch_bytes\":%u}\n",
               mc,(unsigned)dvbs2_fast_mode_size(m),bytes,m->t > 8 ? DVBS2_BCH_CACHE_BYTES : 0);
    }
    assert(!lofty_build(table,128));
    for (unsigned i = 0; i < 1380; ++i) words[i] &= 4095;
    lofty_prepare(offsets,words+1,1365,words[0]);
    offsets[1365] = 0;
    lofty_begin(); lofty_render(got,offsets,1365,table); lofty_end();
#endif
    free(bch); free(map); free(raw); free(payload); free(ws); free(f);
}

#ifdef LOFTY_HOST
int main(void) { kernels(); encoder(); return 0; }
#else
void app_main(void)
{
    puts("QEMU-START");
    if (!LOFTY_TRACE) kernels();
    encoder();
    puts("QEMU-END");
    fflush(stdout);
}
#endif
