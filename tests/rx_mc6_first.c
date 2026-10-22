#include "rx_mc6_first.h"
#define main public_control_main
#include "rx_mc6.c"
#undef main

typedef struct {uint8_t *raw;int16_t *v;} guarded_belief;
static guarded_belief allocate_belief(void)
{
    uint8_t *p=aligned_alloc(16,2U*RX_MC6_N+32);
    assert(p);memset(p,0x6d,2U*RX_MC6_N+32);
    return (guarded_belief){p,(int16_t *)(p+16)};
}
static void intact(guarded_belief b)
{
    for(unsigned n=0;n<16;++n)assert(b.raw[n]==0x6d && b.raw[16+2U*RX_MC6_N+n]==0x6d);
}
static void ownership(void)
{
    guarded_belief b=allocate_belief();int8_t *llr=calloc(RX_MC6_N,1);assert(llr);
    rx_mc6_first d={0},saved=d;
    assert(!rx_mc6_first_init(&d,(int16_t *)((uint8_t *)b.v+2),llr));
    assert(!memcmp(&d,&saved,sizeof d) && b.v[0]==0x6d6d);
    assert(!rx_mc6_first_init(&d,b.v,(int8_t *)b.v));
    assert(!rx_mc6_first_init((rx_mc6_first *)b.v,b.v,llr));
    assert(rx_mc6_first_init(&d,b.v,llr));
    saved=d;assert(!rx_mc6_first_step(&d,0));assert(!memcmp(&d,&saved,sizeof d));
    uint8_t output[8100];memset(output,0x6d,sizeof output);
    assert(!rx_mc6_first_pack(&d,output));for(unsigned n=0;n<8100;++n)assert(output[n]==0x6d);
    assert(rx_mc6_first_step(&d,2746)==2745 && d.complete && !d.failed);
    saved=d;assert(!rx_mc6_first_step(&d,1) && !rx_mc6_first_step_scalar(&d,2745));
    assert(!memcmp(&d,&saved,sizeof d));
    assert(!rx_mc6_first_pack(&d,(uint8_t *)b.v));
    assert(rx_mc6_first_pack(&d,output));for(unsigned n=0;n<8100;++n)assert(!output[n]);
    assert(rx_mc6_first_init(&d,b.v,llr));d.column=1;
    assert(!rx_mc6_first_step_scalar(&d,1) && d.failed);
    intact(b);free(b.raw);free(llr);
}
int main(int argc,char **argv)
{
    ownership();
    int8_t *llr=malloc(RX_MC6_N);rx_mc6_check *checks=malloc(4U*RX_MC6_CHECKS);
    guarded_belief reference_b=allocate_belief(),scalar=allocate_belief(),copy=allocate_belief();
    assert(llr&&checks);
    reference *oracle=NULL;
    bool natural=argc>1 && !strcmp(argv[1],"state");
    if(natural){oracle=calloc(1,sizeof *oracle);assert(oracle);reference_graph(oracle);}
    read_exact(llr,RX_MC6_N);assert(getchar()==EOF);
    memset(checks,0xa5,4U*RX_MC6_CHECKS);
    rx_mc6 general;rx_mc6_first s,c;
    assert(rx_mc6_init(&general,reference_b.v,checks,llr));
    assert(rx_mc6_first_init(&s,scalar.v,llr)&&rx_mc6_first_init(&c,copy.v,llr));
    assert(!memcmp(reference_b.v,scalar.v,2U*RX_MC6_N) && !memcmp(reference_b.v,copy.v,2U*RX_MC6_N));
    if(oracle)for(unsigned n=0;n<RX_MC6_N;++n)oracle->belief[n]=llr[n];
    unsigned wide=argc>2?(unsigned)strtoul(argv[2],NULL,10):0;
    if(wide) {
        for(unsigned n=0;n<RX_MC6_N;++n) {
            int16_t value=wide==1?32767:wide==2?-32768:(int16_t)((n*7919U+137U)&65535U);
            reference_b.v[n]=scalar.v[n]=copy.v[n]=value;
        }
        assert(!oracle);
    }
    for(unsigned tile=0;tile<RX_MC6_TILES;++tile) {
        unsigned layer=general.layer,column=general.column;
        if(oracle)reference_tile(oracle,layer,column);
        assert(rx_mc6_step_blocked(&general,1)==1 && !general.failed);
        assert(rx_mc6_first_step_scalar(&s,1)==1 && !s.failed);
        assert(rx_mc6_first_step(&c,1)==1 && !c.failed);
        assert(!memcmp(reference_b.v,scalar.v,2U*RX_MC6_N));
        assert(!memcmp(reference_b.v,copy.v,2U*RX_MC6_N));
        assert(s.layer==general.layer && s.column==general.column && c.layer==general.layer && c.column==general.column);
        if(oracle)compare(oracle,&general,layer,column);
    }
    assert(s.complete && c.complete && general.iteration==1);
    uint8_t bits[8100],other[8100];assert(rx_mc6_pack(&general,bits));
    assert(rx_mc6_first_pack(&s,other) && !memcmp(bits,other,8100));
    assert(rx_mc6_first_pack(&c,other) && !memcmp(bits,other,8100));
    if(!wide) {
        for(unsigned arm=0;arm<2;++arm) {
            assert(rx_mc6_first_init(&s,scalar.v,llr));
            const unsigned budgets[]={0,1,3,4,89,90,134,2744,2745,7};
            unsigned left=2745;
            for(unsigned n=0;left;++n) {
                unsigned budget=budgets[n%10],expected=budget<left?budget:left;
                rx_mc6_first saved=s;
                unsigned got=arm?rx_mc6_first_step(&s,budget):rx_mc6_first_step_scalar(&s,budget);
                assert(got==expected && !s.failed);
                if(!budget)assert(!memcmp(&saved,&s,sizeof s));
                left-=got;
            }
            assert(s.complete && !memcmp(reference_b.v,scalar.v,2U*RX_MC6_N));
        }
    }
    uint8_t payload[5370];memset(payload,0x6d,sizeof payload);size_t count=0;rx_mc6_result result;
    bool valid=rx_mc6_finish(bits,payload,sizeof payload,&count,&result,RX_MC6_BCH_BYTE);
    assert(fwrite(bits,1,8100,stdout)==8100 && fwrite(payload,1,count,stdout)==count);
    fprintf(stderr,"{\"valid\":%s,\"H\":%u,\"bch\":%s,\"header\":%s,\"bytes\":%zu,\"tiles\":2745,\"state_bytes\":%zu}\n",
            valid?"true":"false",(unsigned)result.unsatisfied,result.bch_ok?"true":"false",result.header_ok?"true":"false",count,sizeof s);
    intact(reference_b);intact(scalar);intact(copy);
    free(llr);free(checks);free(reference_b.raw);free(scalar.raw);free(copy.raw);free(oracle);
    return 0;
}
