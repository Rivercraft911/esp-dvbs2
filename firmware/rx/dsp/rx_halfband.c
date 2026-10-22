/* Independent complex-polyphase implementation of the frozen Q15 filter. */
#include "rx_halfband.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif
volatile uint32_t hb_leaf_max_cycles, hb_leaf_calls;
volatile uint32_t hb_force_spill, hb_forced_spill_calls;

_Static_assert(sizeof(hb_complex)==4, "complex S16 layout");
_Static_assert(sizeof(hb_vector_scratch)==13664, "bounded vector scratch");

static bool IRAM_ATTR valid(const uint32_t *raw,size_t count)
{
#ifdef __XTENSA__
    extern bool scan8_pie(const uint32_t *,size_t);
    while (count && ((uintptr_t)raw&31U)) {
        if (*raw++==HB_SENTINEL) return false;
        --count;
    }
    size_t groups=count/8U;
    while(groups) {
        size_t n=groups<256U?groups:256U;
        if(!scan8_pie(raw,n))return false;
        raw+=n*8U;groups-=n;
    }
    count%=8U;
#endif
    while (count--) if (*raw++==HB_SENTINEL) return false;
    return true;
}

static hb_complex raw_complex(uint32_t word)
{ return (hb_complex){(int16_t)(word>>4)>>6,(int16_t)(word<<6)>>6}; }

static void put(hb_complex *even,hb_complex *odd,unsigned parity,unsigned index,hb_complex z)
{
    int at=(int)index-(int)parity;
    if ((index^parity)&1U) odd[(at-1)/2]=z;
    else even[at/2]=z;
}

static hb_complex get(const hb_complex *even,const hb_complex *odd,unsigned parity,unsigned index)
{
    int at=(int)index-(int)parity;
    return ((index^parity)&1U) ? odd[(at-1)/2] : even[at/2];
}

static void history_in(const hb_history *h,hb_complex *even,hb_complex *odd,unsigned parity)
{
    for (unsigned n=0;n<h->held;++n) put(even,odd,parity,n,(hb_complex){h->i[n],h->q[n]});
}

static void IRAM_ATTR history_out(hb_history *h,const hb_complex *even,const hb_complex *odd,
                         unsigned parity,unsigned total,unsigned added)
{
    unsigned held=total<10U ? total : 10U;
    for (unsigned n=0;n<held;++n) {
        hb_complex z=get(even,odd,parity,total-held+n);
        h->i[n]=z.i;h->q[n]=z.q;
    }
    h->held=held;h->phase=(h->phase+added)&1U;
}

static void IRAM_ATTR unpack(const uint32_t *raw,unsigned count,unsigned physical_left,
                             hb_complex *even,hb_complex *odd,unsigned parity,unsigned *index)
{
    unsigned at=*index;
    while (count && ((at-parity+2U*(((uintptr_t)even&15U)/4U))&7U)) {
        put(even,odd,parity,at++,raw_complex(*raw++));--count;--physical_left;
    }
    unsigned phase=((uintptr_t)raw&15U)/4U;
    unsigned extra=phase ? 4U-phase : 0U;
    unsigned safe=physical_left>=extra ? physical_left-extra : 0U;
    unsigned groups=(count<safe ? count : safe)/8U;
    if (groups) {
        while(groups) {
            unsigned n=groups<64U?groups:64U;
            hb_unpack8(raw,even+(at-parity)/2U,odd+(at-parity)/2U,n);
            unsigned used=n*8U;raw+=used;count-=used;at+=used;groups-=n;
        }
    }
    while (count--) put(even,odd,parity,at++,raw_complex(*raw++));
    *index=at;
}

static inline __attribute__((always_inline)) int fir(const hb_complex *e,const hb_complex *o,unsigned n,bool q)
{
    int a=q ? e[n].q+e[n+5].q : e[n].i+e[n+5].i;
    int b=q ? e[n+1].q+e[n+4].q : e[n+1].i+e[n+4].i;
    int c=q ? e[n+2].q+e[n+3].q : e[n+2].i+e[n+3].i;
    int d=q ? o[n+2].q : o[n+2].i;
    return 274*a-1812*b+9730*c+16384*d;
}

static int8_t clipped(int x)
{ return x < -128 ? -128 : x > 127 ? 127 : (int8_t)x; }

bool IRAM_ATTR hb_vector_bank(hb_tile_state *state,hb_vector_scratch *s,unsigned tile,
                              const uint32_t *bank,unsigned first,size_t pairs,
                              int8_t *out,size_t capacity,size_t *written)
{
    if (!state || !s || !bank || !out || !written || !tile || tile>HB_TILE ||
        first>=HB_BANK_PAIRS || pairs>HB_MAX_PAIRS || capacity<(pairs+3U)/4U ||
        ((uintptr_t)bank&63U) || ((uintptr_t)s&15U)) return false;
    size_t split=HB_BANK_PAIRS-first;if (split>pairs) split=pairs;
    if (!valid(bank+first,split) || !valid(bank,pairs-split)) return false;
    size_t at=0,produced=0;
    while (at<pairs) {
        unsigned count=pairs-at<tile ? (unsigned)(pairs-at) : tile;
        hb_history *a=&state->first,*b=&state->second;
        unsigned parity=(a->phase^a->held)&1U,index=a->held;
        unsigned second_parity=(b->phase^b->held)&1U;
        unsigned prefix=(second_parity-b->held)&3U;
        unsigned phase=(4U-prefix)&3U;
        hb_complex *e=s->even+4U+phase,*o=s->odd+4U+phase;
        hb_complex *e2=s->second_even+4,*o2=s->second_odd+4;
        history_in(a,e,o,parity);history_in(b,e2,o2,second_parity);
        unsigned cursor=(first+at)&(HB_BANK_PAIRS-1U),head=HB_BANK_PAIRS-cursor;
        if (head>count) head=count;
        unpack(bank+cursor,head,HB_BANK_PAIRS-cursor,e,o,parity,&index);
        unpack(bank,count-head,HB_BANK_PAIRS,e,o,parity,&index);
        unsigned start=10U+parity,n1=index>start ? (index-start+1U)/2U : 0U;
        unsigned n=0;
        for (;n<n1 && n<prefix;++n)
            put(e2,o2,second_parity,b->held+n,(hb_complex){(fir(e,o,n,false)+16384)>>15,
                                                         (fir(e,o,n,true)+16384)>>15});
        unsigned groups=(n1-n)/4U;
        while(groups) {
            unsigned g=groups<64U?groups:64U;
            unsigned dest=(b->held+n-second_parity)/2U;
            hb_fir4_15(e+n,o+n,e2+dest,o2+dest,g);n+=4U*g;groups-=g;
        }
        for (;n<n1;++n)
            put(e2,o2,second_parity,b->held+n,(hb_complex){(fir(e,o,n,false)+16384)>>15,
                                                         (fir(e,o,n,true)+16384)>>15});
        history_out(a,e,o,parity,index,count);
        parity=second_parity;index=b->held+n1;e=e2;o=o2;
        start=10U+parity;unsigned n2=index>start ? (index-start+1U)/2U : 0U;
        groups=n2/4U;
        unsigned done=0;
        while(groups) {
            unsigned g=groups<64U?groups:64U;
            hb_fir4_17(e+done,o+done,s->final+2U*done,g);done+=4U*g;groups-=g;
        }
        for (n=done;n<n2;++n) {
            s->final[2U*n]=clipped((fir(e,o,n,false)+65536)>>17);
            s->final[2U*n+1]=clipped((fir(e,o,n,true)+65536)>>17);
        }
        history_out(b,e,o,parity,index,n1);
        memcpy(out+2U*produced,s->final,2U*n2);
        produced+=n2;at+=count;
    }
    *written=produced;return true;
}

/* Each full-state ownership interval clears at most1024 words. */
void IRAM_ATTR rx_halfband_clear(uint32_t *bank)
{
    for(unsigned at=0;at<HB_BANK_PAIRS;at+=1024U)
        rx_pack_clear4(bank+at,256U);
}
