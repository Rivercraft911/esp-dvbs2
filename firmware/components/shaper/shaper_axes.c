// Five distinct axis amplitudes, with the legacy three-tap rounding intact.
#include "vcm_sections.h"
#ifdef __XTENSA__
#define STREAM_PROFILE_FN static VCM_TEXT __attribute__((unused))
#endif
#include "shaper.h"
#include "shaper_taps.h"
#include "esp_attr.h"
#include <assert.h>

static int value(const int32_t *taps, unsigned part, unsigned w, unsigned p)
{
    const int32_t levels[5] = {0, shaper_level_i[1], shaper_level_i[3],
                             shaper_level_i[0], shaper_level_i[4]};
    int64_t sum = 0;
    for (unsigned a = 0; a < 3; ++a, w /= 5)
        sum += (int64_t)levels[w%5] * taps[(3*part+a)*8+p];
    return (int)((sum + ((int64_t)1 << (SHAPER_SHIFT-1))) >> SHAPER_SHIFT);
}

void shaper_axes_build(shaper_table_t *t, uint32_t *store, const int32_t *taps)
{
    uint16_t *rows = (uint16_t *)store;
    uint32_t *map = store + 6144/4;
    const unsigned ci[8] = {3,1,0,2,4,2,0,1}, cq[8] = {0,1,3,1,0,2,4,2};
    for (unsigned p = 0; p < 8; ++p) {
        int bias[3];
        for (unsigned j = 0; j < 3; ++j) {
            int lo = 0;
            for (unsigned w = 0; w < 125; ++w) {
                int v = value(taps,j,w,p);
                if (v < lo) lo = v;
            }
            bias[j] = -lo;
        }
        bias[0] = 512-bias[1]-bias[2];
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned w = 0; w < 125; ++w) {
                int v = value(taps,j,w,p)+bias[j];
                assert(v >= 0 && v < 1024);
                rows[(128*((j+1)%3)+w)*8+p] = (uint16_t)v;
            }
    }
    for (unsigned w = 0; w < 512; ++w) {
        unsigned i = 0, q = 0, place = 1;
        for (unsigned a = 0; a < 3; ++a, place *= 5) {
            unsigned k = (w >> (3*a)) & 7;
            i += ci[k]*place; q += cq[k]*place;
        }
        uintptr_t base=0;
#if UINTPTR_MAX <= UINT32_MAX
        base=(uintptr_t)store;
#endif
        map[2*w] = (uint32_t)(base+2048+16*i);
        map[2*w+1] = (uint32_t)(base+2048+16*q);
    }
    *t = (shaper_table_t){.blocks=store,.n=8,.bits=3,.parts=SHAPER_AXES_PARTS};
}

void shaper_axes_c(uint32_t *dst, const uint8_t *sym, int nsym,
                             const shaper_table_t *t)
{
    if (!nsym) return;
    const uint32_t *map = t->blocks + 6144/4;
    uintptr_t base=0;
#if UINTPTR_MAX > UINT32_MAX
    base=(uintptr_t)t->blocks;
#endif
    uint32_t h = 0;
    for (int a = -8; a < 0; ++a) h = (h << 3) | (sym[a] >> 4);
    for (int s = 0; s < nsym; ++s) {
        h = (h << 3) | (sym[s] >> 4);
        unsigned w[3] = {h&511,(h>>9)&511,(h>>18)&511};
        const int offset[3] = {0,1024,-1024};
        const uint16_t *pi[3], *pq[3];
        for (unsigned j=0;j<3;++j) {
            pi[j]=(const uint16_t *)(base+map[2*w[j]])+offset[j];
            pq[j]=(const uint16_t *)(base+map[2*w[j]+1])+offset[j];
        }
        for (unsigned p = 0; p < 8; ++p) {
            unsigned i = 0, q = 0;
            for (unsigned j = 0; j < 3; ++j) {
                i += pi[j][p];
                q += pq[j][p];
            }
            *dst++ = (i | (q << 10)) ^ SHAPER_XOR;
        }
    }
}

static VCM_TEXT void fill_axes(uint32_t *dst, const uint8_t *sym, int n,
                                const shaper_table_t *t)
{
#ifdef __XTENSA__
    if (!n) return;
    const int groups=(n-1)/6, bulk=6*groups+1;
    shaper_axes8_pie(dst,sym,groups,t->blocks);
    for (int i=bulk;i<n;++i) shaper_axes8_pie(dst+8*i,sym+i,0,t->blocks);
#else
    shaper_axes_c(dst,sym,n,t);
#endif
}

VCM_TEXT void shaper_axes_samples_profile(uint32_t *dst, const uint8_t *sym, unsigned skip,
                                          size_t count, const shaper_table_t *t,
                                          stream_profile_t *whole, stream_profile_t *partial)
{
    if (!count) return;
    uint32_t edge[8] __attribute__((aligned(16)));
    if (skip) {
        const stream_mark_t mark=partial ? stream_mark_for(partial) : (stream_mark_t){0};
        fill_axes(edge,sym++,1,t);
        const size_t take=count < 8-skip ? count : 8-skip;
        for (size_t i=0;i<take;++i) dst[i]=edge[skip+i];
        dst+=take; count-=take;
        if (partial) stream_record(partial,mark,stream_mark_for(partial));
    }
    const size_t symbols=count/8;
    if (symbols) {
        const stream_mark_t mark=whole ? stream_mark_for(whole) : (stream_mark_t){0};
        fill_axes(dst,sym,(int)symbols,t);
        if (whole) stream_record(whole,mark,stream_mark_for(whole));
    }
    dst+=8*symbols; sym+=symbols; count-=8*symbols;
    if (count) {
        const stream_mark_t mark=partial ? stream_mark_for(partial) : (stream_mark_t){0};
        fill_axes(edge,sym,1,t);
        for (size_t i=0;i<count;++i) dst[i]=edge[i];
        if (partial) stream_record(partial,mark,stream_mark_for(partial));
    }
}
