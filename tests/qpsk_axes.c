// QPSK axis queue against the plain encoder, with partial words across frames.
#include "dvbs2.h"
#include "machine.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef QPSK_AXES_HOST
#include "esp_heap_caps.h"
#endif

static void *buffer(size_t n)
{
#ifdef QPSK_AXES_HOST
    return aligned_alloc(16,(n+15)&~(size_t)15);
#else
    return heap_caps_aligned_alloc(16,n,MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static uint32_t advance(uint32_t h, unsigned phase)
{
    return ((h<<1)&0xfffefffe) | (phase == 3 || phase == 5 ? 1 : 0) |
           (phase >= 5 ? 1u<<16 : 0);
}

void qpsk_axes_check(void)
{
    dvbs2_workspace *ws = buffer(sizeof *ws);
    dvbs2_fast *f = buffer(dvbs2_fast_size());
    uint8_t *payload = buffer(8100), *raw = buffer(DVBS2_MAX_SYMBOLS);
    assert(ws && f && payload && raw);
    unsigned cases = 0;
    const unsigned sizes[] = {0,1,2,3,4,7,128,131};
    for (unsigned mc = 4; mc <= 11; ++mc)
        for (unsigned pilots = 0; pilots < 2; ++pilots)
            for (unsigned pie = 0; pie < 2; ++pie) { // the host runs C either way; tests/tx_pie_bench runs PIE
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
                    assert(!dvbs2_fast_axis_start(f,ws,&state));
                    unsigned at = 0;
                    while (state.at < symbols) {
                        uint32_t out[140];
                        for (unsigned j = 0; j < 140; ++j) out[j] = 0xa5a5a5a5;
                        unsigned room = sizes[call%8], offset = 1+(call%4);
                        ++call;
                        unsigned n = dvbs2_fast_axis_read(f,&state,out+offset,room);
                        assert(n <= room);
                        for (unsigned j = 0; j < n; ++j) {
                            do { history = advance(history,raw[at++]); } while (++pending < 16);
                            pending = 0;
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
    free(raw); free(payload); free(f); free(ws);
    printf("axis mapper: %u frames, all QPSK rates/pilots, PIE flags off/on, partial words, guarded slices\n",cases);
}


void machine_expand_check(void)
{
    uint32_t axes[MACHINE_PAGE_AXIS_WORDS], out[MACHINE_PAGE_BLOCKS+2];
    uint32_t history=0, actual=0, x=123;
    for (unsigned page=0;page<100;++page) {
        for (unsigned k=0;k<MACHINE_PAGE_AXIS_WORDS;++k) {
            x^=x<<13; x^=x>>17; x^=x<<5; axes[k]=x;
        }
        out[0]=out[MACHINE_PAGE_BLOCKS+1]=0xdeadbeef;
        actual=machine_expand(out+1,axes,actual);
        unsigned at=0, pending=0;
        for (unsigned k=0;k<MACHINE_PAGE_AXIS_WORDS;++k)
            for (int shift=15;shift>=0;--shift) {
                uint32_t w=axes[k]>>shift;
                history=((history<<1)&0x7ff07ff0)|((w&1)<<4)|((w&0x10000)<<4);
                if (++pending==5) {assert(out[++at]==history);pending=0;}
            }
        assert(actual==history && at==MACHINE_PAGE_BLOCKS);
        assert(out[0]==0xdeadbeef && out[MACHINE_PAGE_BLOCKS+1]==0xdeadbeef);
    }
    puts("axis expansion: 100 pages, persistent history, word alignment, guards: ok");
}


void machine_axis_check(void)
{
    uint32_t *table = buffer(MACHINE_TABLE_WORDS*4);
    uint32_t *expected = buffer(MACHINE_PAGE_SAMPLES*4);
    uint32_t *out = buffer((MACHINE_PAGE_SAMPLES+8)*4);
    uint32_t *scratch = buffer(MACHINE_AXIS_CHUNK_SAMPLES*4);
    assert(table && expected && out && scratch);
    assert(!machine_build(table,128));
    uint32_t x = 123, previous = 0, axes[MACHINE_PAGE_AXIS_WORDS], signs[MACHINE_PAGE_BLOCKS+1];
    unsigned cases = 0;
    for (unsigned page = 0; page < 4; ++page) {
        for (unsigned j = 0; j < MACHINE_PAGE_AXIS_WORDS; ++j) {
            x ^= x<<13; x ^= x>>17; x ^= x<<5; axes[j] = x;
        }
        machine_expand(signs,axes,(previous << 4)&0x7ff07ff0);
        signs[MACHINE_PAGE_BLOCKS] = 0;
        machine_render(expected,signs,MACHINE_PAGE_BLOCKS,table);
        const unsigned skips[] = {0,1,4,11,12,188,191,192,193,384,1020,1344,1535};
        const unsigned counts[] = {0,1,4,11,12,191,192,193,511,768,1536};
        for (unsigned at = 0; at < sizeof skips/sizeof *skips; ++at)
            for (unsigned c = 0; c < sizeof counts/sizeof *counts; ++c)
                for (unsigned offset = 1; offset <= 4; ++offset) {
                    unsigned n = counts[c], skip = skips[at];
                    if (n > MACHINE_PAGE_SAMPLES-skip) n = MACHINE_PAGE_SAMPLES-skip;
                    for (unsigned j = 0; j < MACHINE_PAGE_SAMPLES+8; ++j) out[j] = 0xdeadbeef;
                    machine_axis_samples(out+offset,axes,previous,skip,n,table,scratch);
                    for (unsigned j = 0; j < n; ++j) assert(out[offset+j] == expected[skip+j]);
                    for (unsigned j = 0; j < offset; ++j) assert(out[j] == 0xdeadbeef);
                    for (unsigned j = offset+n; j < MACHINE_PAGE_SAMPLES+8; ++j) assert(out[j] == 0xdeadbeef);
                    ++cases;
                }
        previous = axes[MACHINE_PAGE_AXIS_WORDS-1];
    }
    free(scratch); free(out); free(expected); free(table);
    printf("direct axis renderer: %u guarded slices, all alignments/cuts, persistent history: ok\n",cases);
}

#ifdef QPSK_AXES_HOST
int main(void) { qpsk_axes_check(); machine_expand_check(); machine_axis_check(); return 0; }
#endif
