/* First-sweep NMS with zero old messages and no check-state writes.
 * Tile order, saturation and the arithmetic leaf match the general decoder. */
#include "rx_mc6_first.h"
#include "mc6_internal.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#endif
#define MC6_BCH_ATTR
#include "mc6_tables.h"

static bool separate(const void *a,size_t na,const void *b,size_t nb)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && x<=UINTPTR_MAX-na && y<=UINTPTR_MAX-nb && (x+na<=y || y+nb<=x);
}
bool rx_mc6_first_init(rx_mc6_first *d,int16_t belief[RX_MC6_N],const int8_t llr[RX_MC6_N])
{
    if(!d || ((uintptr_t)belief&15U) || !separate(d,sizeof *d,belief,2U*RX_MC6_N) ||
       !separate(d,sizeof *d,llr,RX_MC6_N) || !separate(belief,2U*RX_MC6_N,llr,RX_MC6_N))return false;
    for(unsigned n=0;n<RX_MC6_K;++n)belief[n]=llr[n];
    for(unsigned layer=0;layer<60;++layer)
        for(unsigned column=0;column<360;++column)
            belief[RX_MC6_K+360U*layer+column]=llr[RX_MC6_K+60U*column+layer];
    *d=(rx_mc6_first){.belief=belief,.initialized=true};
    return true;
}
static unsigned position(unsigned start,unsigned column)
{
    unsigned p=start+column;
    return p<360?p:p-360;
}
static void gather(int16_t out[8],const int16_t *row,unsigned p,unsigned lanes,bool copy)
{
    unsigned head=360U-p;
    if(head>lanes)head=lanes;
    if(copy && head==lanes && !(p&1U)) {
        /* belief/rows are16B-aligned, p even proves4B source alignment;
         * q[e] is16B-aligned internal storage. No unaligned word access. */
        const void *source=__builtin_assume_aligned(row+p,4);
        void *dest=__builtin_assume_aligned(out,16);
        if(lanes==8)__builtin_memcpy(dest,source,16);
        else __builtin_memcpy(dest,source,8);
    } else {
        for(unsigned n=0;n<head;++n)out[n]=row[p+n];
        for(unsigned n=head;n<lanes;++n)out[n]=row[n-head];
    }
    for(unsigned n=lanes;n<8;++n)out[n]=32767;
}
static void scatter(int16_t *row,const int16_t *in,unsigned p,unsigned lanes,bool copy)
{
    unsigned head=360U-p;
    if(head>lanes)head=lanes;
    if(copy && head==lanes && !(p&1U) && (lanes==4 || lanes==8)) {
        /* The degree-nine skip leaves3lanes and takes the scalar path. */
        void *dest=__builtin_assume_aligned(row+p,4);
        const void *source=__builtin_assume_aligned(in,16);
        if(lanes==8)__builtin_memcpy(dest,source,16);
        else __builtin_memcpy(dest,source,8);
    } else {
        for(unsigned n=0;n<head;++n)row[p+n]=in[n];
        for(unsigned n=head;n<lanes;++n)row[n-head]=in[n];
    }
}
static bool valid(rx_mc6_check s,unsigned degree)
{
    unsigned lo=s.magnitudes&255U,hi=s.magnitudes>>8;
    return lo<=hi && hi<=127 && !(s.metadata&0xc000U) &&
           (s.metadata>>10)<degree && (degree==10 || !(s.metadata&512U));
}
static unsigned step(rx_mc6_first *d,unsigned budget,bool copy)
{
    if(!d || !budget || !d->initialized || d->complete || d->failed)return 0;
    if(!d->belief || ((uintptr_t)d->belief&15U) || d->layer>=60 || d->column>=360 ||
       d->column%(d->layer?8U:4U)){d->failed=true;return 0;}
    _Alignas(16) rx_mc6_tile w;
#ifdef ESP_PLATFORM
    if(!esp_ptr_internal(&w) || !esp_ptr_internal((const uint8_t *)&w+sizeof w-1U)) {
        d->failed=true;return 0;
    }
#endif
    for(unsigned n=0;n<4;++n)w.before[n]=w.after[n]=UINT32_C(0x6d6d6d6d);
    unsigned done=0,geometry_layer=60;
    uint16_t base[10],start[10];
    while(done<budget && !d->complete && !d->failed) {
        unsigned layer=d->layer,column=d->column,lanes=layer?8U:4U;
        if(geometry_layer!=layer) {
            for(unsigned e=0;e<8;++e) {
                unsigned token=mc6_graph[layer][e],shift=token&511U;
                base[e]=(uint16_t)(360U*(token>>9));
                start[e]=(uint16_t)(shift?360U-shift:0);
            }
            base[8]=(uint16_t)(RX_MC6_K+360U*layer);start[8]=0;
            base[9]=(uint16_t)(RX_MC6_K+360U*(layer?layer-1U:59U));start[9]=layer?0:359;
            geometry_layer=layer;
        }
        /* First-sweep Rold is identically zero. Neither input compressed
         * metadata nor its zero validation/staging is ever needed here. */
        for(unsigned e=0;e<10;++e)
            gather(w.q[e],d->belief+base[e],position(start[e],column),lanes,copy);
        rx_mc6_block_leaf(&w,1U|(!layer && !column?2U:0U));
        for(unsigned n=0;n<4;++n)
            if(w.before[n]!=UINT32_C(0x6d6d6d6d) || w.after[n]!=UINT32_C(0x6d6d6d6d))d->failed=true;
        if(d->failed)break;
        if(!layer && !column) {
            w.metadata[0]&=(uint16_t)~512U;
            if((w.metadata[0]>>10)==9U)w.metadata[0]=(uint16_t)((w.metadata[0]&1023U)|(8U<<10));
        }
        /* Retain the general decoder's post-leaf metadata sanity checks even
         * though state is never published or consumed by a second sweep. */
        for(unsigned lane=0;lane<lanes;++lane)
            if(!valid((rx_mc6_check){w.magnitudes[lane],w.metadata[lane]},
                      !layer && !column && !lane?9U:10U))d->failed=true;
        if(d->failed)break;
        for(unsigned e=0;e<10;++e) {
            unsigned p=position(start[e],column),skip=!layer && !column && e==9?1U:0U;
            if(skip && ++p==360)p=0;
            scatter(d->belief+base[e],w.q[e]+skip,p,lanes-skip,copy);
        }
        /* Publish the cursor only after completing the tile. */
        ++done;d->column=(uint16_t)(column+lanes);
        if(d->column==360) {
            d->column=0;
            if(++d->layer==60){d->layer=0;d->complete=true;}
        }
    }
    return done;
}
#ifdef RX_MC6_TEST_HOOKS
unsigned rx_mc6_first_step_scalar(rx_mc6_first *d,unsigned budget){return step(d,budget,false);}
#endif
unsigned rx_mc6_first_step(rx_mc6_first *d,unsigned budget){return step(d,budget,true);}
bool rx_mc6_first_pack(const rx_mc6_first *d,uint8_t bits[RX_MC6_WORD_BYTES])
{
    if(!d || !d->initialized || !d->complete || d->failed ||
       !separate(bits,RX_MC6_WORD_BYTES,d,sizeof *d) ||
       !separate(bits,RX_MC6_WORD_BYTES,d->belief,2U*RX_MC6_N))return false;
    memset(bits,0,RX_MC6_WORD_BYTES);
    for(unsigned n=0;n<RX_MC6_N;++n) {
        unsigned at=n<RX_MC6_K?n:RX_MC6_K+360U*((n-RX_MC6_K)%60U)+(n-RX_MC6_K)/60U;
        if(d->belief[at]<0)bits[n/8]|=(uint8_t)(128U>>(n%8U));
    }
    return true;
}
