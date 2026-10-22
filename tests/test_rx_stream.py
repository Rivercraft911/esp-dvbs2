"""Host arithmetic, queue and DMA controls for the finite collector.

The halfband uses two 11-tap stages with shifts 15 and 17. The fixed RRC
uses 65 Q15 taps at 4:1. Native results are recorded in docs/measurements.md.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
APP=ROOT/'tests/rx_stream/main'
DSP=ROOT/'firmware/rx/dsp'
QUEUE=ROOT/'firmware/rx/stream'
ORIGIN={
    'HB_vector':'7111d20d7e320f1c7aa12b2d0fb4696288686d1ac3df2632f5c2917450280ed2',
    'HB_state_wrapper':'921645ba5fe7a866c8ef7b1180c4a8ea19fa95ea3de9d38bf6ef56884404aa0c',
    'source_pin':'eabaef4eed3bca20e8a848b483a5b755da042f59067f46aea262f4f2b94a8bc5',
}
HB_MODEL = r'''
/* Host-only model of the PIE register operations, not the filter oracle. */
#include "rx_halfband.h"
#include <assert.h>
#include <string.h>
#ifndef __XTENSA__
typedef union { uint8_t b[16];uint32_t w[4];int16_t s[8]; } qr;

static qr src(qr low,qr high,unsigned bytes)
{
    uint8_t both[32];memcpy(both,low.b,16);memcpy(both+16,high.b,16);
    qr out;memcpy(out.b,both+bytes,16);return out;
}

static qr load(const void *p)
{ qr q;memcpy(&q,p,16);return q; }

static void unzip32(qr *a,qr *b)
{
    qr x=*a,y=*b;
    for (unsigned n=0;n<2;++n) {
        a->w[n]=x.w[2*n];a->w[n+2]=y.w[2*n];
        b->w[n]=x.w[2*n+1];b->w[n+2]=y.w[2*n+1];
    }
}

static void read8(const void *input,qr *a,qr *b)
{
    uintptr_t address=(uintptr_t)input;unsigned offset=address&15U;
    const uint8_t *base=(const uint8_t *)(address&~(uintptr_t)15U);
    *a=load(base);*b=load(base+16);
    if (offset) {qr c=load(base+32);*a=src(*a,*b,offset);*b=src(*b,c,offset);}
}

void hb_unpack8(const uint32_t *raw,hb_complex *even,hb_complex *odd,size_t groups)
{
    assert(!((uintptr_t)even&15U) && !((uintptr_t)odd&15U));
    while (groups--) {
        qr a,b;read8(raw,&a,&b);
        for (unsigned n=0;n<4;++n) {
            /* VSL.32 then arithmetic VSR.32; unsigned shifts truncate32. */
            uint32_t ai=(uint32_t)((int32_t)(a.w[n]<<12)>>22)&65535U;
            uint32_t bi=(uint32_t)((int32_t)(b.w[n]<<12)>>22)&65535U;
            a.w[n]=(uint32_t)((int32_t)(a.w[n]<<22)>>6)|ai;
            b.w[n]=(uint32_t)((int32_t)(b.w[n]<<22)>>6)|bi;
        }
        unzip32(&a,&b);memcpy(even,&a,16);memcpy(odd,&b,16);
        raw+=8;even+=4;odd+=4;
    }
}

static void mac(int64_t qacc[8],qr q,int coefficient)
{
    for (unsigned n=0;n<8;++n) {
        qacc[n]+=(int64_t)q.s[n]*coefficient;
        assert(qacc[n]>=-(INT64_C(1)<<39) && qacc[n]<(INT64_C(1)<<39));
    }
}

static void accum(const hb_complex *even,const hb_complex *odd,int shift,int64_t out[8])
{
    qr e0=load(even),e4=load(even+4),e8=load(even+8);
    qr e1=src(e0,e4,4),e5=src(e4,e8,4);
    qr o0=load(odd),o4=load(odd+4);
    for (unsigned n=0;n<8;++n) out[n]=shift==15 ? 16384 : 65536;
    mac(out,e0,274);mac(out,e1,-1812);mac(out,e5,274);mac(out,e4,-1812);
    mac(out,src(o0,o4,8),16384);mac(out,src(e0,e4,8),9730);mac(out,src(e0,e4,12),9730);
    for (unsigned n=0;n<8;++n) out[n]>>=shift;
}

/* After SRCMB.S16 shift, split each signed40 lane into signed20 halves.
 * SRCMB.S8 saturates each half, then VUNZIP.8 retains the low half's byte. */
static int8_t extract40(int64_t value)
{
    uint64_t bits=(uint64_t)value&((UINT64_C(1)<<40)-1U);
    int32_t low=(int32_t)((uint32_t)(bits&0xfffffU)<<12)>>12;
    int32_t high=(int32_t)((uint32_t)(bits>>20)<<12)>>12;
    assert(high==0 || high==-1);
    return low < -128 ? -128 : low > 127 ? 127 : (int8_t)low;
}

void hb_fir4_15(const hb_complex *even,const hb_complex *odd,
                 hb_complex *out_even,hb_complex *out_odd,size_t groups)
{
    assert(!((uintptr_t)even&15U) && !((uintptr_t)odd&15U) &&
           !((uintptr_t)out_even&7U) && !((uintptr_t)out_odd&7U));
    while (groups--) {
        int64_t qacc[8];qr q0,q1={{0}};accum(even,odd,15,qacc);
        for (unsigned n=0;n<8;++n) {assert(qacc[n]>=INT16_MIN && qacc[n]<=INT16_MAX);q0.s[n]=(int16_t)qacc[n];}
        unzip32(&q0,&q1);memcpy(out_even,q0.b,8);memcpy(out_odd,q1.b,8);
        even+=4;odd+=4;out_even+=2;out_odd+=2;
    }
}

void hb_fir4_17(const hb_complex *even,const hb_complex *odd,int8_t *out,size_t groups)
{
    assert(!((uintptr_t)even&15U) && !((uintptr_t)odd&15U) && !((uintptr_t)out&7U));
    while (groups--) {
        int64_t qacc[8];accum(even,odd,17,qacc);
        for (unsigned n=0;n<8;++n) out[n]=extract40(qacc[n]);
        even+=4;odd+=4;out+=8;
    }
}

void hb_model_extraction_check(void)
{
    for (int value=INT16_MIN;value<=INT16_MAX;++value)
        assert(extract40(value)==(value < -128 ? -128 : value > 127 ? 127 : value));
}
#endif
'''

HB_REFERENCE = r'''
#include "rx_halfband.h"
static int rounded(int64_t total, unsigned shift)
{
    /* Portable floor after bias, independently spelled without signed shift. */
    total += (int64_t)1 << (shift - 1U);
    int64_t divisor = (int64_t)1 << shift;
    return total >= 0 ? total / divisor : -((-total + divisor - 1) / divisor);
}

size_t hb_reference(const uint32_t *raw, size_t pairs, int16_t (*scratch)[2], int8_t *out)
{
    static const int coefficient[11] = {274,0,-1812,0,9730,16384,9730,0,-1812,0,274};
    size_t first = 0, second = 0;
    for (size_t n = 10; n < pairs; n += 2) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            int64_t total = 0;
            for (unsigned k = 0; k < 11; ++k) {
                unsigned bits = lane ? raw[n-k] & 1023U : (raw[n-k] >> 10) & 1023U;
                int value = bits >= 512U ? (int)bits - 1024 : (int)bits;
                total += coefficient[k] * value;
            }
            scratch[first][lane] = (int16_t)rounded(total, 15);
        }
        ++first;
    }
    for (size_t n = 10; n < first; n += 2) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            int64_t total = 0;
            for (unsigned k = 0; k < 11; ++k) total += coefficient[k] * scratch[n-k][lane];
            int value = rounded(total, 17);
            out[2U * second + lane] = (int8_t)(value < -128 ? -128 : value > 127 ? 127 : value);
        }
        ++second;
    }
    return second;
}
'''

HB_CONTROL = r'''
#include "rx_halfband.h"
extern size_t hb_reference(const uint32_t *,size_t,int16_t (*)[2],int8_t *);
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(64) uint32_t bank[HB_BANK_PAIRS];
static uint32_t sequence[131072];
static int16_t oracle_scratch[65536][2];
static int8_t expected[65536],got[65536+128];
static hb_vector_scratch scratch;
static uint32_t rng=20261020;
static uint32_t random_word(void)
{ rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }

int main(void)
{
    extern void hb_model_extraction_check(void);hb_model_extraction_check();
    static const unsigned cuts[]={1,2,3,5,7,11,13,17,31,64,1023,2049,12288,14288};
    static const unsigned tiles[]={1,3,5,17,512,1024,2048};
    for (unsigned t=0;t<sizeof tiles/sizeof *tiles;++t) {
        for (unsigned trial=0;trial<40;++trial) {
            size_t pairs=trial<32 ? trial : 4096U+random_word()%28672U;
            for (size_t n=0;n<pairs;++n) {
                sequence[n]=random_word();if (sequence[n]==HB_SENTINEL) ++sequence[n];
            }
            size_t count=hb_reference(sequence,pairs,oracle_scratch,expected);
            memset(got,0x6d,sizeof got);memset(&scratch,0xbc,sizeof scratch);
            hb_tile_state state={0};size_t at=0,wrote=0;
            for (unsigned call=0;at<pairs;++call) {
                unsigned first=call<32 ? call*523U%HB_BANK_PAIRS : random_word()%HB_BANK_PAIRS;
                size_t size=cuts[call%(sizeof cuts/sizeof *cuts)];if (size>pairs-at) size=pairs-at;
                for (unsigned n=0;n<HB_BANK_PAIRS;++n) bank[n]=HB_SENTINEL;
                for (size_t n=0;n<size;++n) bank[(first+n)%HB_BANK_PAIRS]=sequence[at+n];
                size_t produced=SIZE_MAX;
                assert(hb_vector_bank(&state,&scratch,tiles[t],bank,first,size,got+64+2*wrote,
                                      (sizeof got-64-2*wrote)/2,&produced));
                wrote+=produced;at+=size;
            }
            assert(wrote==count && !memcmp(expected,got+64,2*count));
            for (unsigned n=0;n<64;++n) assert(got[n]==0x6d);
            for (size_t n=64+2*wrote;n<sizeof got;++n) assert(got[n]==0x6d);
        }
        for (unsigned fault=0;fault<2049;++fault) {
            for (unsigned n=0;n<HB_BANK_PAIRS;++n) bank[n]=n;
            unsigned first=16379;bank[(first+fault)%HB_BANK_PAIRS]=HB_SENTINEL;
            hb_tile_state state={0},before=state;memset(got,0x6d,sizeof got);size_t wrote=123;
            assert(!hb_vector_bank(&state,&scratch,tiles[t],bank,first,2049,got+64,513,&wrote));
            assert(!memcmp(&state,&before,sizeof state) && wrote==123);
            for (unsigned n=0;n<sizeof got;++n) assert(got[n]==0x6d);
        }
    }
    printf("vector operation model vs independent integer oracle: phase/wrap/odd/guards/atomic sentinel pass; scratch=%zu\n",sizeof scratch);
}
'''

HOST_LEAVES = r'''
#include "rx_halfband.h"
#include <stdlib.h>
bool rx_pack_scan8(const uint32_t *p,size_t n){while(n--)for(unsigned k=0;k<8;++k)if(*p++==HB_SENTINEL)return false;return true;}
void rx_pack_clear4(uint32_t *p,size_t n){while(n--)for(unsigned k=0;k<4;++k)*p++=HB_SENTINEL;}
void rx_pack8(const uint32_t *p,uint8_t *q,size_t n){while(n--)for(unsigned k=0;k<8;++k){*q++=(uint8_t)(*p>>12);*q++=(uint8_t)(*p++>>2);}}
void hb_context_call(void (*fn)(void),const uintptr_t args[6],const void *seed,void *before,void *after)
{(void)fn;(void)args;(void)seed;(void)before;(void)after;abort();} /* Never an ISA/context model. */
'''

RRC_DOT_MODEL = r'''
#include "rrc.h"
#include <assert.h>
/* Test-only operation model: pair32 signed-S16 adds/dot plus center, then
 * original bias/floor. Independent65-tap full-window controls are in controls.c. */
void rrc4_batch32(const int16_t *fi,const int16_t *ri,const int16_t *fq,const int16_t *rq,int16_t *out,size_t count)
{
    static const int16_t tap[33]={51,-7,-64,-62,4,69,61,-21,-96,-74,45,151,123,-39,-190,-168,
        33,218,157,-156,-417,-242,420,1071,936,-362,-2215,-3091,-1388,3389,9958,15682,17951};
    while(count--){
        int64_t a=256+tap[32]*(int64_t)fi[32],b=256+tap[32]*(int64_t)fq[32];
        for(unsigned k=0;k<32;++k){
            int x=fi[k]+ri[k],y=fq[k]+rq[k];assert(x>=-32768&&x<=32767&&y>=-32768&&y<=32767);
            a+=(int64_t)tap[k]*x;b+=(int64_t)tap[k]*y;
        }
        out[0]=(int16_t)(a>=0?a/512:-((511-a)/512));
        out[1]=(int16_t)(b>=0?b/512:-((511-b)/512));
        fi+=4;ri-=4;fq+=4;rq-=4;out+=2;
    }
}
'''

RRC_CONTROL = r'''
#include "controls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    _Alignas(64) uint8_t head[64];
    rrc4_tile_partial state;
    uint8_t middle[64];
    _Alignas(16) int16_t output[256];
    uint8_t tail[64];
    _Alignas(64) uint8_t input[RRC_PARTIAL_INPUT_BYTES];
    unsigned calls,pauses,mode;
} fixture;
static bool push(void *opaque,const int8_t *input,size_t pairs,size_t *made)
{
    fixture *f=opaque;++f->calls;
    bool ok=rrc4_tile_partial_push(&f->state,input,pairs,f->output,128,made,true);
    if(f->mode&&f->calls==1) {
        if(f->mode==1)f->output[0]^=1;
        if(f->mode==2)return false;
        if(f->mode==3)++*made;
        if(f->mode==4)f->tail[0]^=1;
        if(f->mode==5)f->input[0]^=1;
        if(f->mode==6)f->state.carry[0][0]^=1;
        if(f->mode==7)f->output[255]^=1;
    }
    return ok;
}
static bool guards(void *opaque)
{
    fixture *f=opaque;
    for(unsigned n=0;n<64;++n)if(f->head[n]!=0x6d||f->middle[n]!=0x6d||f->tail[n]!=0x6d)return false;
    return true;
}
static void pause_case(void *opaque){++((fixture *)opaque)->pauses;}
int main(void)
{
    static fixture f;rrc_partial_report r;unsigned negatives=0;
    for(unsigned mode=0;mode<=7;++mode) {
        memset(&f,0x6d,sizeof f);f.calls=f.pauses=0;f.mode=mode;
        bool ok=rrc_partial_controls(&f.state,f.input,f.output,push,guards,pause_case,&f,&r);
        assert(r.measured);
        if(!mode) {
            assert(ok&&r.cases==192&&r.components==32640&&r.crc32==785216665U&&!r.errors);
            assert(r.guards&&r.context&&f.calls==192&&f.pauses==192&&guards(&f));
        } else {
            assert(!ok&&r.errors==1&&r.first_case==0&&f.calls==1&&f.pauses==0);
            if(mode==1)assert(r.expected!=r.actual&&r.first_component==0);
            if(mode==2)assert(!r.context);
            if(mode>=4)assert(!r.guards);
            ++negatives;
        }
    }
    memset(&r,0x6d,sizeof r);rrc_partial_report before=r;
    assert(!rrc_partial_controls(NULL,f.input,f.output,push,guards,pause_case,&f,&r));
    assert(!memcmp(&r,&before,sizeof r));
    printf("{\"native_protocol_host_cases\":192,\"components\":32640,\"S16LE_crc32\":785216665,\"negative_groups\":%u,\"invalid_argument_refusal\":true,\"errors\":0}\n",negatives);
    return 0;
}
'''

QUEUE_CONTROL = r'''
#define _POSIX_C_SOURCE 200809L
#include "rx_queue.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ch_queue q;ch_pipe pipe;uint8_t *arena,*stages;
    const uint8_t *source;uint8_t *dest;unsigned bytes;
    bool dma,adc,close_fails,finish_fails,filter_fails,begin_fails,clear_fails;
    uint64_t output;uint32_t clock,cost;
    unsigned filters,finishes,begins,clears,stops,closes;
    char events[4096];size_t event_count;
} fixture;
static void event(fixture *f,char e){assert(f->event_count<sizeof f->events);f->events[f->event_count++]=e;}
static uint8_t value(uint64_t n){return (uint8_t)((n*137U)^(n>>8));}
static bool filter(void *v,unsigned stage,unsigned count,uint8_t *out,unsigned expected)
{
    fixture *f=v;(void)stage;assert(count>=CH_UNIT-CH_LATE&&count<=CH_UNIT+CH_LATE);event(f,'F');++f->filters;
    if(f->filter_fails)return false;
    for(unsigned n=0;n<2U*expected;++n)out[n]=value(2U*f->output+n);
    f->output+=expected;return true;
}
static bool finish(void *v)
{
    fixture *f=v;event(f,'D');++f->finishes;assert(f->dma);
    if(f->finish_fails)return false;
    memcpy(f->dest,f->source,f->bytes);f->dma=false;return true;
}
static bool begin(void *v,const uint8_t *s,unsigned bytes,uint8_t *d)
{
    fixture *f=v;event(f,'B');++f->begins;assert(!f->dma&&f->adc);
    assert(((uintptr_t)s&63U)==((uintptr_t)d&63U));
    if(f->begin_fails)return false;
    f->dma=true;f->source=s;f->dest=d;f->bytes=bytes;return true;
}
static bool clear(void *v){fixture *f=v;event(f,'C');++f->clears;return !f->clear_fails;}
static void stop(void *v){fixture *f=v;event(f,'S');++f->stops;f->adc=false;}
static bool close_dma(void *v){fixture *f=v;event(f,'X');++f->closes;assert(!f->adc);if(f->close_fails)return false;f->dma=false;return true;}
static uint32_t cycles(void *v){fixture *f=v;uint32_t t=f->clock;f->clock+=f->cost;return t;}
static const ch_ops ops={filter,finish,begin,clear,stop,close_dma,cycles};
static void init(fixture *f,unsigned slots,uint32_t seed)
{
    memset(f,0,sizeof *f);assert(!posix_memalign((void **)&f->arena,64,slots*CH_SLOT_BYTES));
    assert(!posix_memalign((void **)&f->stages,64,2U*CH_SLOT_BYTES));
    memset(f->arena,0xa5,slots*CH_SLOT_BYTES);memset(f->stages,0xa5,2U*CH_SLOT_BYTES);
    assert(ch_init(&f->q,f->arena,slots*CH_SLOT_BYTES,slots,seed));
    assert(ch_pipe_init(&f->pipe,&f->q,f->stages,&ops,f));f->adc=true;f->cost=130000;
}
static void drop(fixture *f)
{assert(ch_reclaimable(&f->q)&&f->pipe.dma_idle&&!f->dma);free(f->stages);free(f->arena);}
static unsigned drain(fixture *f,uint64_t *input,uint64_t *output)
{
    unsigned got=0;ch_lease x;
    while(ch_acquire(&f->q,&x)==CH_DATA) {
        assert(x.meta.input_first==*input&&x.meta.output_first==*output);
        for(unsigned n=0;n<2U*x.meta.output_pairs;++n)assert(x.data[n]==value(2U**output+n));
        *input+=x.meta.input_pairs;*output+=x.meta.output_pairs;
        assert(ch_guards(&f->q,&x));assert(ch_release(&f->q,&x));++got;
    }
    return got;
}
int main(void)
{
    unsigned cases=0;fixture f;ch_lease x,y;uint64_t input=0,output=0;
    init(&f,8,UINT32_MAX-2U);assert(ch_pipe_step(&f.pipe,CH_UNIT));
    assert(ch_acquire(&f.q,&x)==CH_EMPTY); /* Pending DMA is never advertised. */
    for(unsigned k=1;k<80;++k) {
        assert(ch_pipe_step(&f.pipe,CH_UNIT+(k%5)*127));
        assert(ch_pipe_boundary(&f.pipe,131722,1145,true));
        if(k%3==0)assert(drain(&f,&input,&output)>0);
    }
    assert(ch_pipe_stop(&f.pipe,true));drain(&f,&input,&output);
    assert(input==f.q.input_pairs&&output==(input-27U)/4U&&output==f.q.output_pairs);
    assert(ch_acquire(&f.q,&x)==CH_END&&ch_reader_finish(&f.q));drop(&f);++cases;

    /* Reader retains a zero-copy slot: writer fills others but never overwrites it. */
    init(&f,4,0);assert(ch_pipe_step(&f.pipe,CH_UNIT)&&ch_pipe_step(&f.pipe,CH_UNIT));
    assert(ch_acquire(&f.q,&x)==CH_DATA);uint8_t saved[6144];assert(2U*x.meta.output_pairs<=sizeof saved);
    memcpy(saved,x.data,2U*x.meta.output_pairs);assert(ch_acquire(&f.q,&y)==CH_ERROR);
    assert(ch_pipe_step(&f.pipe,CH_UNIT)&&ch_pipe_step(&f.pipe,CH_UNIT));
    assert(!ch_pipe_step(&f.pipe,CH_UNIT)&&f.stops==1&&f.closes==1);
    assert(!memcmp(saved,x.data,2U*x.meta.output_pairs));assert(!ch_reclaimable(&f.q));
    assert(ch_release(&f.q,&x)&&!ch_release(&f.q,&x));
    assert(ch_acquire(&f.q,&x)==CH_ERROR&&ch_reader_finish(&f.q));drop(&f);++cases;

    /* Bad cache/EOF/guard paths cannot publish unfinished chunks, and stop
     * precedes close/free on every early error. */
    for(unsigned fault=0;fault<9;++fault) {
        init(&f,4,0);assert(ch_pipe_step(&f.pipe,CH_UNIT));
        if(fault==0)f.filter_fails=true;
        if(fault==1)f.finish_fails=true;
        if(fault==2)f.begin_fails=true;
        if(fault==3)f.clear_fails=true;
        if(fault==4)f.pipe.pending_source[-1]^=1;
        if(fault==5)f.pipe.pending.data[2U*f.pipe.pending.meta.output_pairs]^=1;
        if(fault==6)f.cost=CH_SERVICE_LIMIT+1U;
        if(fault==7)f.clock=UINT32_MAX-500U; /* unsigned CCOUNT rollover is valid */
        if(fault==8)f.close_fails=true,f.finish_fails=true;
        bool ok=ch_pipe_step(&f.pipe,CH_UNIT);
        if(fault==7)assert(ok&&ch_pipe_stop(&f.pipe,true));else assert(!ok);
        assert(f.stops==1&&f.closes==1);
        char *st=memchr(f.events,'S',f.event_count),*cl=memchr(f.events,'X',f.event_count);assert(st&&cl&&st<cl);
        assert(ch_acquire(&f.q,&x)==(fault==7?CH_DATA:CH_ERROR));
        if(fault==7){assert(ch_release(&f.q,&x));input=CH_UNIT;output=(CH_UNIT-27U)/4U;drain(&f,&input,&output);}
        assert(ch_reader_finish(&f.q));
        if(fault==8){assert(!ch_reclaimable(&f.q));free(f.stages);free(f.arena);} /* simulator teardown only, real buffers quarantined */
        else drop(&f);
        ++cases;
    }
    for(unsigned fault=0;fault<3;++fault) {
        init(&f,4,0);assert(ch_pipe_step(&f.pipe,CH_UNIT));
        assert(!ch_pipe_boundary(&f.pipe,fault==0?CH_SERVICE_LIMIT+1:131722,
                    fault==1?CH_WINDOW_LIMIT+1:1145,fault!=2));
        assert(ch_reader_finish(&f.q));drop(&f);++cases;
    }
    init(&f,4,0);assert(!ch_reserve(&f.q,CH_UNIT-CH_LATE-1,&x));
    assert(!ch_reserve(&f.q,CH_UNIT+CH_LATE+1,&x));assert(f.q.reserved==0&&f.q.input_pairs==0);
    assert(ch_reserve(&f.q,CH_UNIT,&x));assert(ch_reserve(&f.q,CH_UNIT,&y));
    ch_lease z;assert(!ch_reserve(&f.q,CH_UNIT,&z));
    assert(!ch_publish(&f.q,&y));assert(!ch_reader_finish(&f.q));
    assert(!ch_release(&f.q,&x));assert(!ch_pipe_stop(&f.pipe,false));assert(ch_reader_finish(&f.q));drop(&f);++cases;
    /* Graceful stop waits for the reader, and cannot re-open the session. */
    init(&f,4,0);assert(ch_pipe_step(&f.pipe,CH_UNIT));assert(ch_pipe_stop(&f.pipe,true));
    assert(!ch_reclaimable(&f.q)&&!ch_pipe_step(&f.pipe,CH_UNIT));input=output=0;
    assert(drain(&f,&input,&output)==1);assert(ch_reader_finish(&f.q));drop(&f);++cases;
    init(&f,4,0);assert(ch_pipe_step(&f.pipe,CH_UNIT)&&ch_pipe_step(&f.pipe,CH_UNIT));
    assert(ch_acquire(&f.q,&x)==CH_DATA);ch_reader_reject(&f.q);
    assert(!ch_reader_finish(&f.q));assert(ch_release(&f.q,&x));
    assert(ch_reader_finish(&f.q)&&!ch_reclaimable(&f.q));
    assert(!ch_pipe_step(&f.pipe,CH_UNIT));drop(&f);++cases;
    ch_position pos={.pairs=UINT32_MAX-4U,.target=(uint64_t)UINT32_MAX+CH_UNIT,
                     .cycle=UINT32_MAX-99U,.changed=UINT32_MAX-99U,.index=16380};
    assert(ch_observe(&pos,50,8)&&pos.pairs==(uint64_t)UINT32_MAX+8U);
    ch_position saved_pos=pos;
    assert(!ch_observe(&pos,50+CH_POLL_LIMIT,9)&&!memcmp(&pos,&saved_pos,sizeof pos));
    assert(!ch_observe(&pos,60,16384)&&!memcmp(&pos,&saved_pos,sizeof pos));
    pos.changed=pos.cycle-245700U;assert(!ch_observe(&pos,pos.cycle+100U,pos.index));
    pos=(ch_position){.pairs=UINT64_MAX-1U,.index=0};
    assert(!ch_observe(&pos,1,2));++cases;
    printf("{\"cases\":%u,\"host_queue_bytes\":%zu,\"host_pipe_bytes\":%zu,\"host_meta_bytes\":%zu,\"slot_bytes\":%u,\"PSRAM64slots_bytes\":%u,\"DMA_publication_and_borrow_lifetime\":true,\"ticket_and_CCOUNT_rollover\":true,\"quarantine_on_failed_DMA_stop\":true}\n",cases,sizeof(ch_queue),sizeof(ch_pipe),sizeof(ch_meta),CH_SLOT_BYTES,CH_MAX_SLOTS*CH_SLOT_BYTES);
}
'''

THREAD_CONTROL = r'''
#define _POSIX_C_SOURCE 200809L
#include "rx_queue.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>

static ch_queue q;
static unsigned pauses;
static uint8_t sample(uint64_t at){return (uint8_t)(at^(at>>8)^(at>>16));}
static void *reader(void *unused)
{
    (void)unused;uint64_t input=0,output=0;unsigned read=0;ch_lease x;
    for(;;) {
        enum ch_read result=ch_acquire(&q,&x);
        if(result==CH_EMPTY){sched_yield();continue;}
        if(result==CH_END)break;
        assert(result==CH_DATA&&x.meta.input_first==input&&x.meta.output_first==output);
        if(read%7U==0){++pauses;sched_yield();}
        for(unsigned n=0;n<2U*x.meta.output_pairs;++n)assert(x.data[n]==sample(2U*output+n));
        input+=x.meta.input_pairs;output+=x.meta.output_pairs;assert(ch_release(&q,&x));++read;
    }
    assert(read==1024&&output==(input-27U)/4U&&ch_reader_finish(&q));return NULL;
}
int main(void)
{
    void *arena;assert(!posix_memalign(&arena,64,CH_MAX_SLOTS*CH_SLOT_BYTES));
    assert(ch_init(&q,arena,CH_MAX_SLOTS*CH_SLOT_BYTES,CH_MAX_SLOTS,UINT32_MAX-31U));
    pthread_t thread;assert(!pthread_create(&thread,NULL,reader,NULL));
    for(unsigned n=0;n<1024;++n) {
        ch_lease x;unsigned count=CH_UNIT-CH_LATE+(n*137U)%4001U;
        /* Test driver retries to explore interleavings. Actual ADC adapter
         * NEVER waits for space: ch_pipe_step stops on queue exhaustion. */
        while(!ch_reserve(&q,count,&x))sched_yield();
        for(unsigned k=0;k<2U*x.meta.output_pairs;++k)x.data[k]=sample(2U*x.meta.output_first+k);
        assert(ch_publish(&q,&x));
    }
    ch_end_stream(&q,true);assert(!pthread_join(thread,NULL));assert(ch_reclaimable(&q));free(arena);
    printf("{\"real_threads\":2,\"chunks\":1024,\"reader_pauses\":%u,\"ticket_rollover\":true,\"all_bytes_and_global_positions_exact\":true}\n",pauses);
}
'''

CONTEXT_CONTROL = r'''
/* Diagnostic bookkeeping only. These stubs do not model Xtensa instructions. */
#include <assert.h>
#include <string.h>
#include "controls.c"
volatile uint32_t hb_leaf_max_cycles,hb_leaf_calls,hb_force_spill,hb_forced_spill_calls;
bool rx_pack_scan8(const uint32_t *p,size_t n){(void)p;(void)n;return true;}
void rx_pack_clear4(uint32_t *p,size_t n){(void)p;(void)n;}
void rx_pack8(const uint32_t *p,uint8_t *q,size_t n){(void)p;(void)q;(void)n;}
void hb_unpack8(const uint32_t *p,hb_complex *q,hb_complex *r,size_t n){(void)p;(void)q;(void)r;(void)n;}
void hb_fir4_15(const hb_complex *p,const hb_complex *q,hb_complex *r,hb_complex *s,size_t n)
{(void)p;(void)q;(void)r;(void)s;(void)n;}
void hb_fir4_17(const hb_complex *p,const hb_complex *q,int8_t *r,size_t n){(void)p;(void)q;(void)r;(void)n;}
static unsigned scenario,calls;
void hb_context_call(void (*fn)(void),const uintptr_t args[6],const void *seed,void *before,void *after)
{
    (void)fn;(void)args;
    unsigned trial=calls++%12U;
    uint32_t *b=before,*a=after;
    memcpy(b,seed,224);b[55]=0x0006002fU;memcpy(a,b,224);
    if(hb_force_spill)hb_forced_spill_calls+=trial%3U?2U:1U;
    if(scenario==1)a[55]^=3U<<16; /* Ignored ABI CALLINC still retained raw. */
    if(scenario==2&&hb_force_spill&&trial%3U)a[55]^=1U<<8;
    if(scenario==3){a[0]^=1;a[31]^=2;a[32]^=4;a[54]^=8;a[55]^=16;}
    if(scenario==4&&calls==72U)a[36]^=0x80000000U;
}
int main(void)
{
    static _Alignas(16) uint32_t raw[16384];
    static hb_vector_scratch scratch;
    hb_context_report r;
    for(scenario=0;scenario<5;++scenario){
        calls=0;bool ok=hb_context_controls(raw,&scratch,&r);
        assert(r.cases==72&&r.forced_calls==60&&hb_force_spill==0);
        if(scenario<=1){assert(ok&&!r.errors&&!r.mismatch_words[0]&&!r.mismatch_words[1]);}
        if(scenario==1)assert(r.xor_words[55]==3U<<16);
        if(scenario==2){
            assert(ok&&!r.errors&&r.raw_difference_cases==24&&r.first_leaf==0&&r.first_trial==7);
            assert(!r.mismatch_words[0]&&!r.mismatch_words[1]);
            assert(r.xor_words[55]==1U<<8);
            assert(r.first_before[55]==0x0006002fU&&r.first_after[55]==0x0006012fU);
            for(unsigned n=0;n<72;++n){
                unsigned trial=n%12U;
                assert(!r.case_words[n][0]);
                assert(r.case_words[n][1]==(trial>=6U&&trial%3U?1U<<23:0));
            }
        }
        if(scenario==3){
            assert(!ok&&r.errors==72&&r.mismatch_words[0]==0x80000001U);
            assert(r.mismatch_words[1]==0x00c00001U);
            assert(r.first_leaf==0&&r.first_trial==0&&r.first_before[0]!=r.first_after[0]);
        }
        if(scenario==4){
            assert(!ok&&r.errors==1&&r.first_leaf==5&&r.first_trial==11);
            assert(!r.mismatch_words[0]&&r.mismatch_words[1]==16U);
            assert(r.xor_words[36]==0x80000000U&&r.case_words[71][1]==16U);
        }
    }
    /* Independent PS field classification: only OWB[11:8] and CALLINC[17:16]
     * are ABI history. Flipping every other PS bit must still fail. */
    for(unsigned bit=0;bit<32;++bit){
        uint32_t before[56]={0},after[56]={0};memset(&r,0,sizeof r);
        after[55]=1U<<bit;record_context(&r,0,0,before,after);
        bool abi=(bit>=8&&bit<=11)||bit==16||bit==17;
        assert(r.errors==(abi?0U:1U)&&r.xor_words[55]==1U<<bit);
    }
    for(unsigned word=0;word<55;++word){
        uint32_t before[56]={0},after[56]={0};memset(&r,0,sizeof r);
        after[word]=0x80000001U;record_context(&r,0,0,before,after);
        assert(r.errors==1&&r.mismatch_words[word/32U]==1U<<(word%32U));
    }
    return 0;
}
'''

def compile_run(folder,name,inputs):
    exe=folder/name
    args=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
          '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all',
          '-fno-omit-frame-pointer','-pthread','-I'+str(APP),'-I'+str(DSP),'-I'+str(QUEUE),
          *map(str,inputs),'-o',str(exe)]
    subprocess.run(args,check=True)
    subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))

with tempfile.TemporaryDirectory(prefix='rx-stream-host-') as temp:
    folder=Path(temp)
    files={}
    for name,code in [('model',HB_MODEL),('reference',HB_REFERENCE),('hb',HB_CONTROL),('leaves',HOST_LEAVES),
                      ('dot',RRC_DOT_MODEL),('rrc',RRC_CONTROL),('queue',QUEUE_CONTROL),('thread',THREAD_CONTROL),('context',CONTEXT_CONTROL)]:
        p=folder/(name+'.c');p.write_text(code);files[name]=p
    compile_run(folder,'halfband',[files['hb'],files['model'],files['reference'],files['leaves'],DSP/'rx_halfband.c'])
    compile_run(folder,'queue',[files['queue'],QUEUE/'rx_queue.c'])
    compile_run(folder,'thread',[files['thread'],QUEUE/'rx_queue.c'])
    compile_run(folder,'rrc',[files['rrc'],files['dot'],files['model'],files['leaves'],APP/'rrc.c',APP/'controls.c',DSP/'rx_halfband.c'])
    compile_run(folder,'context',[files['context'],files['dot'],APP/'rrc.c'])

def function(text,name):
    import re
    m=re.search(r'(?m)^(?:static )?[^\n;]*\b'+name+r'\([^;]*?\)\n\{',text)
    assert m,name
    end=text.index('{',m.start())+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[m.start():end]
header=(APP/'dma.h').read_text()
struct=header[header.index('typedef struct {'):header.index('} rx_spill;')+len('} rx_spill;')]
source=(APP/'dma.c').read_text()
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define IRAM_ATTR
#define RX_PACKED_CAPTURE 1
#define COPY_TIMEOUT 244800U
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT -2
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 1
#define pdTRUE 1
#define pdFALSE 0
#define GDMA_LL_EVENT_RX_SUC_EOF 1U
#define GDMA_LL_EVENT_RX_ERR_EOF 2U
#define GDMA_LL_EVENT_RX_DESC_ERROR 4U
#define GDMA_LL_EVENT_RX_DESC_EMPTY 8U
#define RX_ERRORS 30U
#define TX_ERRORS 4U
#define GDMA_LLI_OWNER_CPU 0
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_NO_MEM -4
typedef int esp_err_t,BaseType_t,gdma_lli_owner_t;
typedef void *gdma_channel_handle_t,*gdma_link_list_handle_t;
typedef unsigned StaticSemaphore_t;
typedef unsigned *SemaphoreHandle_t;
typedef struct {intptr_t rx_eof_desc_addr;struct{unsigned normal_eof:1,abnormal_eof:1;}flags;} gdma_event_data_t;
static unsigned mode,clock32,sem,barriers,caches;static int GDMA;
'''
prefix+=struct+r'''
static rx_spill *current;
static uint32_t esp_cpu_get_cycle_count(void){clock32+=10;return clock32;}
static void gdma_ll_rx_stop(void *p,int c){(void)p;(void)c;}
static void gdma_ll_tx_stop(void *p,int c){(void)p;(void)c;}
static bool gdma_ll_rx_is_desc_fsm_idle(void *p,int c){(void)p;(void)c;return mode!=13;}
static bool gdma_ll_tx_is_desc_fsm_idle(void *p,int c){(void)p;(void)c;return true;}
static void esp_psram_mspi_mb(void){++barriers;}
static int xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *wake){*wake=0;if(*s)return 0;*s=1;return 1;}
static bool eof_event(gdma_channel_handle_t,gdma_event_data_t *,void *);
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned ticks){
 if(ticks&&mode==1&&!*s){gdma_event_data_t e={.rx_eof_desc_addr=1234,.flags.normal_eof=1};eof_event(NULL,&e,current);}
 if(!*s)return 0;*s=0;return 1;
}
static uint32_t gdma_ll_rx_get_interrupt_status(void *p,int c,bool raw){(void)p;(void)c;(void)raw;return mode==10?16U:0;}
static uint32_t gdma_ll_tx_get_interrupt_status(void *p,int c,bool raw){(void)p;(void)c;(void)raw;return mode==11?4U:0;}
static uint32_t gdma_ll_rx_get_success_eof_desc_addr(void *p,int c){(void)p;(void)c;return mode==6?4321:1234;}
static uint32_t gdma_link_get_item_addr(void *p,int n){(void)p;(void)n;return 1234;}
static int gdma_link_get_owner(void *p,int n,gdma_lli_owner_t *owner){(void)p;(void)n;*owner=mode==7?1:0;return mode==8?ESP_FAIL:ESP_OK;}
static size_t gdma_link_get_length(void *p,int n){(void)p;(void)n;return mode==9?2000:2016;}
static int esp_cache_msync(void *p,size_t bytes,int flags){(void)p;assert(bytes==4032&&flags==1);++caches;return mode==12?ESP_FAIL:ESP_OK;}
'''
text=prefix+'\n'.join(function(source,n) for n in ['stop','notify','eof_event','descriptor_error','descriptor_empty','rx_spill_finish'])+r'''
int main(void){
 for(mode=0;mode<16;++mode){
  rx_spill s={.bytes=4032,.rx_end=1,.active=true,.notifications=true,.eof=&sem};current=&s;
  clock32=sem=barriers=caches=0;s.started_cycle=esp_cpu_get_cycle_count();
  if(mode!=1&&mode!=2){
   gdma_event_data_t e={.rx_eof_desc_addr=mode==5?4321:1234,.flags.normal_eof=mode!=3,.flags.abnormal_eof=mode==3};
   eof_event(NULL,&e,&s);
   if(mode==4)eof_event(NULL,&e,&s);
   if(mode==14)descriptor_error(NULL,NULL,&s);
   if(mode==15)descriptor_empty(NULL,NULL,&s);
  }
  int result=rx_spill_finish(&s);
  if(mode<=1){assert(result==ESP_OK&&!s.active&&caches==1&&barriers==1);assert(s.max_completion_cycles&&s.max_transfer_cycles&&s.max_wait_cycles);}
  else assert(result!=ESP_OK);
  if(mode==1)assert(s.blocked_waits==1&&s.max_blocked_wait_cycles&&s.total_blocked_wait_cycles);
  if(mode==13)assert(s.active); /* failed idle proof quarantines storage */
  if(mode==4)assert(s.notification_errors==1);
 }
 puts("actual EOF/finish controls: 2 success and 14 failure paths pass; no scheduler/IRQ timing claim");
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'test.c').write_text(text)
    subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))

# Compile the actual public console emitters, with real report headers, under
# host format warnings. SDK identity/clock calls are not executed or simulated.
console=(APP/'main.c').read_text()
wire_prefix=r'''
#include <stdio.h>
#include <string.h>
#include "collector.h"
static struct {uint32_t before[4],raw[512],after[4];} snapshot;
static rx_stream_report last;static bool valid;
static const char *esp_err_to_name(int code){(void)code;return "ESP_OK";}
'''
wire_source=wire_prefix+'\n'.join(function(console,n) for n in
    ['snapshot_guards','qualified','cycles_text','leaf_result','report'])+r'''
int main(void){
 valid=qualified();report();
 last.capture.ready_free_internal=135000;last.capture.ready_largest_internal=129584;
 last.capture.ready_free_psram=7340032;last.capture.ready_largest_psram=4194240;
 last.capture.ready_memory_measured=true;report();
 last.capture.leaf_context.cases=72;last.capture.leaf_context.forced_calls=60;
 last.capture.leaf_context.complete=true;leaf_result();return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rx-stream-wire-') as temp:
    folder=Path(temp);(folder/'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n')
    p=folder/'wire.c';p.write_text(wire_source);exe=folder/'wire'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wformat=2',
        '-fsanitize=address,undefined','-I'+str(folder),'-I'+str(APP),'-I'+str(DSP),'-I'+str(QUEUE),
        '-I'+str(ROOT/'firmware/rx/main'),str(p),'-o',str(exe)],check=True)
    rows=[json.loads(line[1:]) for line in subprocess.check_output([str(exe)],text=True).splitlines()]
    assert rows[0]['cmd']=='stream' and rows[0]['complete'] is False
    assert rows[-1]['cmd']=='stream_leaf_context' and rows[-1]['complete'] is True
    assert rows[-1]['PS_excluded_mask']==0x30f00 and rows[-1]['cases']==72
    assert rows[-1]['QR_QACC_ACCX_SAR_SAR_BYTE_CPE_restored'] is True
    memory=[row for row in rows if row['cmd']=='stream_memory']
    assert len(memory)==2 and all(row['phase']=='ready_before_ADC' for row in memory)
    keys=['free_internal_8bit','largest_internal_8bit','free_psram_8bit','largest_psram_8bit']
    assert memory[0]['measured'] is False and all(memory[0][key] is None for key in keys)
    assert memory[1]['measured'] is True
    assert [memory[1][key] for key in keys]==[135000,129584,7340032,4194240]

# One allocation-free snapshot after worker/EOF admission, before capture.
collector=(APP/'collector.c').read_text()
worker=function(collector,'worker')
snapshot=worker[worker.index('const unsigned internal_caps'):worker.index('s->error = collect(r);')]
assert 'MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT' in snapshot
assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in snapshot
assert snapshot.count('heap_caps_get_free_size(')==2
assert snapshot.count('heap_caps_get_largest_free_block(')==2
assert snapshot.count('s->ready_memory_measured = true;')==1
assert worker.index('stream_open(r)')<worker.index('rx_spill_notifications(r->spill)')
assert worker.index('rx_spill_notifications(r->spill)')<worker.index('tick_hook = s->tick_hook_registered = true;')
assert worker.index('tick_hook = s->tick_hook_registered = true;')<worker.index('const unsigned internal_caps')
assert 'heap_caps_' not in function(collector,'collect')

# The two causal assembly regressions: locals must precede the32B call8
# spill area, and each counter must be addressed by its own symbol.
def safe(frame,local_end):return frame%16==0 and local_end<=frame-32
assert not safe(256,244) and not safe(288,276)
assembly=(DSP/'rx_halfband.S').read_text()
entries=[int(x) for x in re.findall(r'\bentry\s+a1,\s*(\d+)',assembly)]
assert len(entries)==5 and all(safe(x,244) for x in entries)
assert 'fir hb_fir4_15,15' in assembly and 'fir hb_fir4_17,17' in assembly
control=(APP/'controls.S').read_text()
assert re.search(r'\bentry\s+a1,\s*320\b',control) and safe(320,276)
assert 'call8 xthal_window_spill' in control
wrapper=(DSP/'rx_halfband_context.inc').read_text()
assert 'movi a10,hb_leaf_calls' in wrapper and 'movi a10,hb_leaf_max_cycles' in wrapper
assert not re.search(r'[ls]32i\s+a9,a10,4',wrapper)
assert 'call8 hb_forced_window_spill' in wrapper
assert '(15U<<8)' in (APP/'controls.c').read_text()
print(json.dumps(dict(host_only=True,origin=ORIGIN,ASan_UBSan=True,native_link_context_and_timing_required=True)))
