#include "rrc.h"

#include <string.h>

#ifdef __XTENSA__
#define HOT __attribute__((section(".iram1")))
#define DATA __attribute__((section(".dram1"),aligned(16)))
#else
#define HOT
#define DATA
#endif

static const int16_t tap[33] DATA={
    51,-7,-64,-62,4,69,61,-21,-96,-74,45,151,123,-39,-190,-168,
    33,218,157,-156,-417,-242,420,1071,936,-362,-2215,-3091,
    -1388,3389,9958,15682,17951
};
_Static_assert((RRC4_TILE_PAIRS+64)%8==0,"each plane starts16aligned");
_Static_assert(sizeof(rrc4_tile_partial)==4752,"bounded tile and64 real carry pairs");

bool rrc4_tile_partial_init(rrc4_tile_partial *s,unsigned phase) {
    if(!s || (uintptr_t)s%16 || phase>3)return false;
    memset(s,0,sizeof *s);s->phase=phase;return true;
}
static inline void put(rrc4_tile_partial *s,unsigned length,unsigned at,int8_t i,int8_t q) {
    s->plane[0][at]=s->plane[2][length-1-at]=i;
    s->plane[1][at]=s->plane[3][length-1-at]=q;
}
/* Only the physical mirror span is rounded. Prefix and tail keep every
 * source load inside the real input span, even when
 * the two physical VLDs used by SRC.Q would otherwise read outside it. */
static HOT void prepare(rrc4_tile_partial *s,unsigned length,unsigned first,
                        const int8_t *iq,size_t pairs,bool vector) {
    rrc4_unpack_span plan={pairs,0,0};
    if(vector)rrc4_unpack_plan((uintptr_t)iq,first,length,pairs,&plan);
    size_t n=0;
    for(;n<plan.prefix;++n)put(s,length,first+(unsigned)n,iq[2*n],iq[2*n+1]);
    if(plan.groups) {
        unsigned at=first+(unsigned)n;
        rrc4_unpack8(iq+2*n,s->plane[0]+at,s->plane[1]+at,
                     s->plane[2]+length-at-8,s->plane[3]+length-at-8,plan.groups);
        n+=8*plan.groups;
    }
    for(;n<pairs;++n)put(s,length,first+(unsigned)n,iq[2*n],iq[2*n+1]);
}
static int16_t rounded(int32_t a) {
    int32_t v=a+256;
    return (int16_t)(v>=0?v/512:-((511-v)/512));
}
HOT bool rrc4_tile_partial_push(rrc4_tile_partial *s,const int8_t *iq,size_t pairs,
                   int16_t *out,size_t capacity,size_t *written,bool vector) {
    if(!s || (uintptr_t)s%16 || !written || s->phase>3 || pairs>RRC4_TILE_PAIRS ||
       pairs>UINT64_MAX-s->total || (pairs && !iq))return false;
    unsigned carry=s->total<64?(unsigned)s->total:64;
    unsigned length=carry+(unsigned)pairs;
    unsigned physical=(length+7U)&~7U;
    /* start is a global input-window index.64 is divisible by4, so its phase
     * is also the selected endpoint phase. Carry contains only real samples. */
    unsigned first=(s->phase-(unsigned)((s->total-carry)&3U))&3U;
    size_t count=length>=65+first?1+(length-65-first)/4:0;
    if(count>capacity || (count && !out))return false;
    if(!pairs){*written=0;return true;}
    prepare(s,physical,0,&s->carry[0][0],carry,vector);
    prepare(s,physical,carry,iq,pairs,vector);
    if(vector && count) {
        unsigned right=physical-65-first;
        rrc4_batch32(s->plane[0]+first,s->plane[2]+right,
                     s->plane[1]+first,s->plane[3]+right,out,count);
    } else for(size_t n=0;n<count;++n) {
        unsigned left=first+4*(unsigned)n,right=physical-65-left;
        int32_t a[2]={0,0};
        for(unsigned k=0;k<32;++k) {
            a[0]+=tap[k]*(s->plane[0][left+k]+s->plane[2][right+k]);
            a[1]+=tap[k]*(s->plane[1][left+k]+s->plane[3][right+k]);
        }
        out[2*n]=rounded(a[0]+tap[32]*s->plane[0][left+32]);
        out[2*n+1]=rounded(a[1]+tap[32]*s->plane[1][left+32]);
    }
    unsigned keep=length<64?length:64;
    for(unsigned n=0;n<keep;++n) {
        s->carry[n][0]=(int8_t)s->plane[0][length-keep+n];
        s->carry[n][1]=(int8_t)s->plane[1][length-keep+n];
    }
    s->total+=pairs;*written=count;return true;
}

HOT void rrc4_unpack_plan(uintptr_t input,unsigned first,unsigned length,size_t pairs,
                         rrc4_unpack_span *p) {
    *p=(rrc4_unpack_span){pairs,0,0};
    if(length%8 || !pairs)return;
    size_t prefix=(8U-(first&7U))&7U;
    if(prefix>pairs)return;
    /* With a nonaligned source, keep one complete scalar group before the
     * rounded-down first VLD. This works even at odd input-byte addresses. */
    if((input+2*prefix)&15U)prefix+=8;
    if(prefix>pairs)return;
    uintptr_t source=input+2*prefix,end=input+2*pairs;
    size_t groups=(pairs-prefix)/8;
    if(source&15U) {
        uintptr_t low=source&~(uintptr_t)15;
        size_t bounded=end-low>=32?(end-low-16)/16:0;
        if(groups>bounded)groups=bounded;
    }
    *p=(rrc4_unpack_span){prefix,groups,pairs-prefix-8*groups};
}

#ifndef __XTENSA__
/* Independent instruction-operation model. ZIP/UNZIP use simultaneous input
 * copies. Only low8 bytes survive signextension; stale upper lanes are ignored.
 * FFT.VST.R32(shift0) reverses32-bit words, preserving eachword's two halfwords. */
static void unzip(uint8_t a[16],uint8_t b[16],unsigned bytes) {
    uint8_t old[32];memcpy(old,a,16);memcpy(old+16,b,16);
    for(unsigned n=0;n<16/bytes;++n) {
        memcpy(a+n*bytes,old+2*n*bytes,bytes);
        memcpy(b+n*bytes,old+(2*n+1)*bytes,bytes);
    }
}
static void zip(uint8_t a[16],uint8_t b[16],unsigned bytes) {
    uint8_t old[32],out[32];memcpy(old,a,16);memcpy(old+16,b,16);
    for(unsigned n=0;n<16/bytes;++n) {
        memcpy(out+2*n*bytes,old+n*bytes,bytes);
        memcpy(out+(2*n+1)*bytes,old+16+n*bytes,bytes);
    }
    memcpy(a,out,16);memcpy(b,out+16,16);
}
void rrc4_unpack8(const int8_t *iq,int16_t *fi,int16_t *fq,int16_t *ri,int16_t *rq,size_t groups) {
    uint8_t q0[16],q1[16]={0},q2[16],q3[16],physical[32];
    uintptr_t source=(uintptr_t)iq,reverse_i=(uintptr_t)ri,reverse_q=(uintptr_t)rq;
    for(size_t n=0;n<groups;++n) {
        unsigned phase=source&15U;
        memcpy(physical,(const void *)(source&~(uintptr_t)15),phase?32:16);
        memcpy(q0,physical+phase,16);
        unzip(q0,q1,1);
        for(unsigned k=0;k<16;++k) {
            q2[k]=(q0[k]&128)?255:0;q3[k]=(q1[k]&128)?255:0;
        }
        zip(q0,q2,1);zip(q1,q3,1);
        memcpy(fi+8*n,q0,16);memcpy(fq+8*n,q1,16);
        unzip(q0,q1,2);zip(q1,q0,2);
        for(unsigned word=0;word<4;++word) {
            memcpy((void *)(reverse_i+4*word),q1+4*(3-word),4);
            memcpy((void *)(reverse_q+4*word),q0+4*(3-word),4);
        }
        source+=16;reverse_i-=16;reverse_q-=16;
    }
}
#endif
