#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shaper_apsk.h"
#include "fixture.h"
static uint32_t table[32768] __attribute__((aligned(16)));
static uint32_t single[4608] __attribute__((aligned(16)));
static uint32_t tail[288] __attribute__((aligned(16)));
static uint32_t want[2048], got[2056] __attribute__((aligned(16)));
static uint8_t sym[264];
static unsigned checks, failures;
static uint32_t hash(const uint32_t *p,int n) { uint32_t h=2166136261u; while(n--) h=(h^*p++)*16777619u; return h; }
static void single_pie(const struct fixture *f,int g) {
    if(f->n==4) { if(f->frac) apsk_single_n4_f3(got+4,sym+8,g,single); else apsk_single_n4_f0(got+4,sym+8,g,single); }
    else { if(f->frac) apsk_single_n8_f3(got+4,sym+8,g,single); else apsk_single_n8_f0(got+4,sym+8,g,single); }
}
#define PAIR(M,N,F) apsk_pair_m##M##_n##N##_f##F(got+4,sym+8,g,table,tail)
static void pair_pie(const struct fixture *f,int g) {
    if(f->m==16) {
        if(f->n==4) { if(f->frac) PAIR(16,4,3); else PAIR(16,4,0); }
        else { if(f->frac) PAIR(16,8,3); else PAIR(16,8,0); }
    } else {
        if(f->n==4) { if(f->frac) PAIR(32,4,3); else PAIR(32,4,0); }
        else { if(f->frac) PAIR(32,8,3); else PAIR(32,8,0); }
    }
}
void app_main(void) {
    for(int c=0;c<sizeof(fixtures)/sizeof(*fixtures);++c) {
        const struct fixture *f=&fixtures[c];
        apsk_build_single(single,f->taps,f->levels,f->m,f->n,f->frac);
        apsk_build_pair(table,tail,f->taps,f->levels,f->m,f->n,f->frac);
        int bad= hash(single,(f->m+4)*16*f->n)!=f->single || hash(table,f->m*f->m*4*f->n)!=f->pair || hash(tail,(f->m+4)*f->n)!=f->tail;
        if(bad) { printf("BAD BUILD %d\n",c); failures++; }
        for(int pair=0;pair<2;++pair) for(int pattern=0;pattern<5;++pattern) for(int t=0;t<4;++t) {
            int groups[]={0,1,2,17}; int g=groups[t], count=g*(pair?7:9), words=count*f->n;
            uint32_t x=2026+pattern;
            for(int i=0;i<count+8;++i) {
                x^=x<<13; x^=x>>17; x^=x<<5;
                sym[i]=pattern==0 ? x%f->m : pattern==1 ? i%f->m : pattern==2 ? f->m-1 : pattern==3 ? 0 : (pair ? (i*7)%f->m : f->m+i%4);
            }
            memset(got,0x5a,sizeof(got));
            if(pair) {
                apsk_pair_c(want,sym+8,count,table,tail,f->m,f->n,f->frac);
                pair_pie(f,g);
            } else {
                apsk_single_c(want,sym+8,count,single,f->n,f->frac);
                single_pie(f,g);
            }
            int first=-1;
            for(int i=0;i<words;++i) if(got[4+i]!=want[i]) { first=i; break; }
            int guard=0;
            for(int i=0;i<4;++i) guard |= got[i]!=0x5a5a5a5a || got[4+words+i]!=0x5a5a5a5a;
            if(first>=0 || guard) {
                failures++; if(failures<12) printf("BAD c%d pair%d pat%d groups%d at%d want%08lx got%08lx guard%d\n",c,pair,pattern,g,first,(unsigned long)(first<0?0:want[first]),(unsigned long)(first<0?0:got[4+first]),guard);
            }
            checks++;
        }
        printf("CASE %d m%d n%d f%d checks%u failures%u\n",c,f->m,f->n,f->frac,checks,failures);
    }
    printf("QEMU-END checks%u failures%u\n",checks,failures);
    for(;;) vTaskDelay(1000);
}
