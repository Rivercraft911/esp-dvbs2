// Streaming mapper against the plain encoder, including unfinished groups at EOF.
#include "dvbs2.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SIGN_HOST
#include "esp_heap_caps.h"
#endif

static void *buffer(size_t n)
{
#ifdef SIGN_HOST
    return aligned_alloc(16,(n+15)&~(size_t)15);
#else
    return heap_caps_aligned_alloc(16,n,MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static uint32_t advance(uint32_t h, unsigned phase)
{
    return ((h<<1)&0x7ff07ff0) | (phase == 3 || phase == 5 ? 16 : 0) |
           (phase >= 5 ? 1u<<20 : 0);
}

void sign_mapper_check(void)
{
    dvbs2_workspace *ws = buffer(sizeof *ws);
    dvbs2_fast *f = buffer(dvbs2_fast_size());
    uint8_t *payload = buffer(8100), *raw = buffer(DVBS2_MAX_SYMBOLS);
    uint32_t *lut = buffer(4096);
    assert(ws && f && payload && raw && lut);
    for (unsigned w = 0; w < 1024; ++w) {
        uint32_t h = 0;
        for (int j = 4; j >= 0; --j) h = advance(h,2*((w>>(2*j))&3)+1);
        lut[w] = h;
    }
    unsigned cases = 0;
    const unsigned sizes[] = {0,1,2,3,4,7,128,131};
    for (unsigned mc = 4; mc <= 11; ++mc)
        for (unsigned pilots = 0; pilots < 2; ++pilots)
            for (unsigned pie = 0; pie < 2; ++pie) {
                const dvbs2_mode *mode = dvbs2_find_mode(mc);
                const unsigned bytes = (mode->kbch-80)/8, symbols = dvbs2_frame_symbols(mode,pilots);
                dvbs2_sign_state state = {0};
                uint32_t history = 0, x = 2026+mc;
                unsigned pending = 0, call = 0;
                for (unsigned frame = 0; frame < 3; ++frame) {
                    for (unsigned j = 0; j < bytes; ++j) {
                        x ^= x<<13; x ^= x>>17; x ^= x<<5;
                        payload[j] = x;
                    }
                    assert(!dvbs2_fast_prepare(f,mode,pilots,DVBS2_MAP_QUADS | (pie ? 15 : 0)));
                    assert(!dvbs2_encode_frame(f,ws,NULL,payload,bytes*8,raw));
                    // ws still contains the same FEC bits. The next map may
                    // replace scratch, but must preserve the FEC and its tail.
                    assert(!dvbs2_fast_sign_start(f,ws,&state));
                    unsigned at = 0;
                    while (state.at < symbols) {
                        uint32_t out[140];
                        for (unsigned j = 0; j < 140; ++j) out[j] = 0xa5a5a5a5;
                        unsigned room = sizes[call%8], offset = 1+(call%4);
                        ++call;
                        unsigned n = dvbs2_fast_sign_read(f,&state,out+offset,room,lut);
                        assert(n <= room);
                        for (unsigned j = 0; j < n; ++j) {
                            do { history = advance(history,raw[at++]); } while (++pending < 5);
                            pending = 0;
                            if (out[offset+j] != history)
                                printf("sign mc %u pilots %u pie %u frame %u at %u offset %u j %u: %08x != %08x\n",
                                       mc,pilots,pie,frame,at,offset,j,(unsigned)out[offset+j],(unsigned)history);
                            assert(out[offset+j] == history);
                        }
                        for (unsigned j = 0; j < offset; ++j) assert(out[j] == 0xa5a5a5a5);
                        for (unsigned j = offset+n; j < 140; ++j) assert(out[j] == 0xa5a5a5a5);
                    }
                    while (at < symbols) { history = advance(history,raw[at++]); ++pending; }
                    assert(state.pending == pending && state.history == history);
                    ++cases;
                }
            }
    free(lut); free(raw); free(payload); free(f); free(ws);
    printf("sign mapper: %u frames, all QPSK rates/pilots, C/PIE, partial groups, guarded slices\n",cases);
}

#ifdef SIGN_HOST
int main(void) { sign_mapper_check(); return 0; }
#endif
