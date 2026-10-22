#include "controls.h"

#include <string.h>
extern bool rx_pack_scan8(const uint32_t *,size_t);
extern void rx_pack_clear4(uint32_t *,size_t);
extern void rx_pack8(const uint32_t *,uint8_t *,size_t);
extern void hb_context_call(void (*)(void),const uintptr_t[6],const void *,void *,void *);
static void record_context(hb_context_report *r,unsigned leaf,unsigned trial,
                           const uint32_t before[HB_CONTEXT_WORDS],
                           const uint32_t after[HB_CONTEXT_WORDS])
{
    unsigned which=r->cases++;
    bool raw_difference=false,different=false;
    for(unsigned word=0;word<HB_CONTEXT_WORDS;++word){
        uint32_t raw_xor=before[word]^after[word];
        r->xor_words[word]|=raw_xor;
        /* CALLINC is call history; RETW underflow updates OWB after the leaf
         * restores PS (Cadence ISA8.3.253/254,8.3.265). Native forced-spill
         * diagnostics measured only OWB. All other PS/PIE bits stay strict. */
        uint32_t raw=word==55U?raw_xor&~(3U<<16):raw_xor;
        uint32_t compared=word==55U?raw&~(15U<<8):raw;
        if(raw){
            uint32_t bit=1U<<(word%32U);
            r->case_words[which][word/32U]|=bit;
            raw_difference=true;
        }
        if(compared){r->mismatch_words[word/32U]|=1U<<(word%32U);different=true;}
    }
    if(raw_difference){
        if(!r->raw_difference_cases){
            r->first_leaf=leaf;r->first_trial=trial;
            memcpy(r->first_before,before,sizeof r->first_before);
            memcpy(r->first_after,after,sizeof r->first_after);
        }
        ++r->raw_difference_cases;
    }
    if(different)++r->errors;
}
bool hb_context_controls(uint32_t *raw,hb_vector_scratch *scratch,hb_context_report *r)
{
    _Alignas(16) uint32_t seed[56],before[56],after[56];
    uint32_t random=20261022U;memset(r,0,sizeof *r);hb_forced_spill_calls=0;
    void (*const leaves[])(void)={(void (*)(void))rx_pack_scan8,(void (*)(void))rx_pack_clear4,
        (void (*)(void))rx_pack8,(void (*)(void))hb_unpack8,
        (void (*)(void))hb_fir4_15,(void (*)(void))hb_fir4_17};
    for(unsigned leaf=0;leaf<6;++leaf)for(unsigned trial=0;trial<12;++trial) {
        for(unsigned n=0;n<56;++n){random^=random<<13;random^=random>>17;random^=random<<5;seed[n]=random;}
        for(unsigned n=0;n<1024;++n)raw[n]=n*17401U+3U;
        memset(scratch,0x13,sizeof *scratch);memset(before,0,sizeof before);memset(after,0,sizeof after);
        unsigned groups=trial%3U==0?0U:trial%3U==1?1U:64U;
        if(leaf==0&&trial>=8U&&groups)raw[0]=HB_SENTINEL;
        uintptr_t args[6]={(uintptr_t)raw,groups,0,0,0,0};
        if(leaf==1&&groups)args[1]=256U;
        if(leaf==2){args[1]=(uintptr_t)scratch->final;args[2]=groups;}
        if(leaf==3){args[1]=(uintptr_t)scratch->even;args[2]=(uintptr_t)scratch->odd;args[3]=groups;}
        if(leaf>=4){args[0]=(uintptr_t)scratch->even;args[1]=(uintptr_t)scratch->odd;
            args[2]=(uintptr_t)scratch->second_even;
            if(leaf==4){args[3]=(uintptr_t)scratch->second_odd;args[4]=groups;}else args[3]=groups;}
        hb_force_spill=trial>=6U;
        hb_context_call(leaves[leaf],args,seed,before,after);
        record_context(r,leaf,trial,before,after);
    }
    hb_force_spill=0;r->forced_calls=hb_forced_spill_calls;
    r->complete=r->cases==72U&&r->errors==0&&r->forced_calls==60U;return r->complete;
}

#include <limits.h>
#include <string.h>

/* Independent full-window equation, not the symmetric target dot schedule. */
static const int16_t tap[65]={
    51,-7,-64,-62,4,69,61,-21,-96,-74,45,151,123,-39,-190,-168,
    33,218,157,-156,-417,-242,420,1071,936,-362,-2215,-3091,
    -1388,3389,9958,15682,17951,15682,9958,3389,-1388,-3091,
    -2215,-362,936,1071,420,-242,-417,-156,157,218,33,-168,
    -190,-39,123,151,45,-74,-96,-21,61,69,4,-62,-64,-7,51
};
static int8_t sample(unsigned j)
{
    unsigned v=(73U*j+151U*(j>>8))&255U;
    return (int8_t)(v<128U?(int)v:(int)v-256);
}
static int16_t expected(unsigned first,unsigned channel)
{
    int64_t sum=256;
    for(unsigned k=0;k<65;++k)sum+=(int64_t)tap[k]*sample(2U*(first+k)+channel);
    return (int16_t)(sum>=0?sum/512:-((511-sum)/512));
}
static uint32_t crc_word(uint32_t crc,int16_t word)
{
    uint16_t u=(uint16_t)word;crc=~crc;
    for(unsigned n=0;n<2;++n) {
        crc^=(u>>(8U*n))&255U;
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1U)?UINT32_C(0xedb88320):0U);
    }
    return ~crc;
}
static void fail(rrc_partial_report *r,unsigned component,int wanted,int actual)
{
    if(!r->errors){r->first_case=r->cases;r->first_component=component;r->expected=wanted;r->actual=actual;}
    ++r->errors;
}
bool rrc_partial_controls(rrc4_tile_partial *s,uint8_t input[RRC_PARTIAL_INPUT_BYTES],int16_t out[256],
                          rrc_partial_push push,rrc_partial_guard guard,
                          rrc_partial_pause pause,void *opaque,rrc_partial_report *r)
{
    if(!s||!input||!out||!push||!guard||!pause||!r)return false;
    memset(r,0,sizeof *r);r->measured=r->guards=r->context=true;
    for(unsigned warm=0;warm<2;++warm)for(unsigned phase=0;phase<4;++phase)
    for(unsigned offset=0;offset<16;++offset)for(unsigned variant=0;variant<(warm?2U:1U);++variant) {
        unsigned pairs=warm?(variant?511U:505U):65U+phase,carry=warm?64U:0U;
        if(!rrc4_tile_partial_init(s,phase)){fail(r,0,1,0);return false;}
        for(unsigned q=0;q<4;++q)for(unsigned n=0;n<576;++n)
            s->plane[q][n]=(int16_t)((n+q+pairs+offset)&1U?INT16_MIN:INT16_MAX);
        s->total=carry;
        for(unsigned n=0;n<64;++n)for(unsigned q=0;q<2;++q)s->carry[n][q]=sample(2U*n+q);
        memset(input,0x6d,RRC_PARTIAL_INPUT_BYTES);memset(out,0x6d,512);
        uint8_t *raw=input+32U+offset;
        for(unsigned n=0;n<2U*pairs;++n)raw[n]=(uint8_t)sample(2U*carry+n);
        size_t made=999;
        bool ok=push(opaque,(const int8_t *)raw,pairs,&made);
        if(!ok){r->context=false;fail(r,0,1,0);return false;}
        size_t count=(carry+pairs>=65U+phase)?1U+(carry+pairs-65U-phase)/4U:0U;
        if(made!=count){fail(r,0,(int)count,(int)made);return false;}
        for(unsigned k=0;k<2U*made;++k) {
            int16_t want=expected(phase+4U*(k/2U),k&1U);
            if(out[k]!=want){fail(r,k,want,out[k]);return false;}
            r->crc32=crc_word(r->crc32,out[k]);++r->components;
        }
        bool intact=guard(opaque)&&s->total==carry+pairs&&s->phase==phase;
        for(unsigned n=0;n<RRC_PARTIAL_INPUT_BYTES;++n) {
            bool real=n>=32U+offset&&n<32U+offset+2U*pairs;
            uint8_t want=real?(uint8_t)sample(2U*carry+n-32U-offset):0x6d;
            if(input[n]!=want)intact=false;
        }
        for(unsigned n=2U*(unsigned)made;n<256;++n)if(out[n]!=(int16_t)0x6d6d)intact=false;
        unsigned keep=carry+pairs<64U?carry+pairs:64U;
        for(unsigned n=0;n<keep;++n)for(unsigned q=0;q<2;++q)
            if(s->carry[n][q]!=sample(2U*(carry+pairs-keep+n)+q))intact=false;
        if(!intact){r->guards=false;fail(r,0,1,0);return false;}
        ++r->cases;pause(opaque);
    }
    if(r->cases!=RRC_PARTIAL_CASES||r->components!=RRC_PARTIAL_COMPONENTS||r->crc32!=RRC_PARTIAL_EXPECTED_CRC) {
        fail(r,0,(int)RRC_PARTIAL_EXPECTED_CRC,(int)r->crc32);return false;
    }
    return true;
}
