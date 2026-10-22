/* Radio-off first-sweep comparison with PSRAM operands. The clean case
 * deliberately runs a sweep for equality, rather than timing the fast path.
 * Allocation, CRCs, comparisons, reports and yields are outside work clocks. */
#include "rx_mc6_first.h"
#include "dvbs2.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {uint8_t *raw;size_t bytes;} first_buffer;
static first_buffer allocate(size_t n)
{
    first_buffer b={heap_caps_aligned_alloc(16,n+32,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),n};
    if(b.raw)memset(b.raw,0x6d,n+32);
    return b;
}
static bool intact(first_buffer b)
{
    if(!b.raw || !esp_ptr_external_ram(b.raw) || !esp_ptr_external_ram(b.raw+b.bytes+31))return false;
    for(unsigned n=0;n<16;++n)if(b.raw[n]!=0x6d || b.raw[16+b.bytes+n]!=0x6d)return false;
    return true;
}
static const char *yes(bool x){return x?"true":"false";}
static unsigned cursor(unsigned layer,unsigned column,bool complete)
{
    return complete?2745:layer?90+(layer-1)*45+column/8:column/4;
}
static bool first_controls(first_buffer belief,first_buffer snapshot,first_buffer channel,first_buffer packed)
{
    first_buffer checks=allocate(86400);
    bool ok=intact(checks);
    int8_t *llr=(int8_t *)(channel.raw+16);
    int16_t *v=(int16_t *)(belief.raw+16);
    memset(llr,0,64800);
    const unsigned at[8][2]={{0,0},{0,4},{0,88},{0,356},{1,0},{1,352},{59,0},{59,352}};
    unsigned cases=0;
    for(unsigned pattern=0;ok && pattern<3;++pattern)
        for(unsigned tile=0;ok && tile<8;++tile)
            for(unsigned arm=0;ok && arm<3;++arm) {
                rx_mc6 d={0};rx_mc6_first f={0};
                ok=arm?rx_mc6_first_init(&f,v,llr):rx_mc6_init(&d,v,(rx_mc6_check *)(checks.raw+16),llr);
                for(unsigned n=0;n<64800;++n)
                    v[n]=pattern==0?32767:pattern==1?-32768:(int16_t)((7919U*n+137U)&65535U);
                if(arm){f.layer=at[tile][0];f.column=at[tile][1];}
                else{d.layer=at[tile][0];d.column=at[tile][1];}
                unsigned got=arm==0?rx_mc6_step_blocked(&d,1):arm==1?rx_mc6_first_step_scalar(&f,1):rx_mc6_first_step(&f,1);
                ok=ok && got==1 && !(arm?f.failed:d.failed);
                if(!arm)memcpy(snapshot.raw+16,v,129600);
                else ok=ok && !memcmp(snapshot.raw+16,v,129600);
                ok=ok && intact(belief) && intact(snapshot) && intact(channel) && intact(checks);
                ++cases;
                vTaskDelay(1);
            }
    heap_caps_free(checks.raw);
    /* Zero-step/cancel-before-publication is checked without another sweep. */
    rx_mc6_first f={0};
    ok=ok && rx_mc6_first_init(&f,v,llr);
    rx_mc6_first saved=f;
    bool zero_step=!rx_mc6_first_step_scalar(&f,0) && !rx_mc6_first_step(&f,0) && !memcmp(&saved,&f,sizeof f);
    uint8_t *refused=packed.raw+16;memset(refused,0x6d,8100);
    bool partial_refused=!rx_mc6_first_pack(&f,refused);
    for(unsigned n=0;n<8100;++n)partial_refused=partial_refused && refused[n]==0x6d;
    ok=ok && zero_step && partial_refused && intact(packed);
    printf("@{\"cmd\":\"mc6_first_controls\",\"tile_cases\":%u,\"full_S16_patterns\":3,"
           "\"arms\":3,\"zero_step\":%s,\"partial_pack_refusal\":%s,\"PSRAM_IO\":true,\"passed\":%s}\n",
           cases,yes(zero_step),yes(partial_refused),yes(ok));
    return ok && cases==72;
}

bool mc6_first_bench(void)
{
    first_buffer belief=allocate(129600),snapshot=allocate(129600),channel=allocate(64800);
    first_buffer packed=allocate(8100),payload=allocate(5370),truth=allocate(5370),word=allocate(8100);
    first_buffer encoded=allocate(sizeof(dvbs2_workspace));
    bool ok=intact(belief)&&intact(snapshot)&&intact(channel)&&intact(packed)&&intact(payload)&&intact(truth)&&intact(word)&&intact(encoded);
    ok=ok && esp_clk_cpu_freq()==240000000 && xPortGetCoreID()==0;
    if(!ok)goto done;
    if(!first_controls(belief,snapshot,channel,packed)){ok=false;goto done;}
    dvbs2_workspace *encoder=(dvbs2_workspace *)(encoded.raw+16);
    const unsigned flips[3]={0,9,80};
    const uint32_t expected[3]={4240430337U,2144775371U,1400211940U};
    int16_t *v=(int16_t *)(belief.raw+16);
    int8_t *llr=(int8_t *)(channel.raw+16);
    uint8_t *bits=packed.raw+16,*out=payload.raw+16,*contents=truth.raw+16;
    for(unsigned c=0;ok && c<3;++c) {
        for(unsigned n=0;n<5370;++n)contents[n]=(uint8_t)(17*n+31*c+3);
        const dvbs2_mode *mode=dvbs2_find_mode(6);
        ok=mode && dvbs2_bb_gcs(encoder,mode,contents,8*5370)==0;
        if(!ok)break;
        dvbs2_bb_scramble(encoder,mode);dvbs2_bch(encoder,mode);dvbs2_ldpc(encoder,mode);
        memcpy(word.raw+16,encoder->bits,8100);
        ok=esp_rom_crc32_le(0,word.raw+16,8100)==expected[c];
        for(unsigned n=0;n<64800;++n)llr[n]=(word.raw[16+n/8]&(128U>>(n%8)))?-24:24;
        for(unsigned n=0;n<flips[c];++n){unsigned at=(7919*n+137*c)%64800;llr[at]=llr[at]<0?8:-8;}
        uint32_t immutable=esp_rom_crc32_le(0,(const uint8_t *)llr,64800);
        for(unsigned trial=0;ok && trial<2;++trial)
            for(unsigned order=0;ok && order<3;++order) {
                unsigned arm=trial?2-order:order;
                /* Only the general arm allocates checks; free before another arm. */
                first_buffer checks=arm?(first_buffer){0}:allocate(86400);
                rx_mc6 d={0};rx_mc6_first f={0};
                bool stage=arm || intact(checks);
                if(!stage){ok=false;heap_caps_free(checks.raw);break;}
                uint32_t t=esp_cpu_get_cycle_count();
                stage=arm?rx_mc6_first_init(&f,v,llr):rx_mc6_init(&d,v,(rx_mc6_check *)(checks.raw+16),llr);
                uint32_t init=esp_cpu_get_cycle_count()-t;
                uint64_t steps=0;int64_t wait=0;unsigned tiles=0,calls=0;
                while(stage && tiles<2745) {
                    unsigned budget=2745-tiles;if(budget>64)budget=64;
                    t=esp_cpu_get_cycle_count();
                    unsigned got=arm==0?rx_mc6_step_blocked(&d,budget):arm==1?rx_mc6_first_step_scalar(&f,budget):rx_mc6_first_step(&f,budget);
                    steps+=(uint32_t)(esp_cpu_get_cycle_count()-t);
                    tiles+=got;++calls;
                    stage=got==budget && !(arm?f.failed:d.failed) &&
                          cursor(arm?f.layer:d.layer,arm?f.column:d.column,arm?f.complete:d.iteration==1)==tiles;
                    int64_t before=esp_timer_get_time();vTaskDelay(1);wait+=esp_timer_get_time()-before;
                }
                t=esp_cpu_get_cycle_count();
                bool packed_ok=arm?rx_mc6_first_pack(&f,bits):rx_mc6_pack(&d,bits);
                uint32_t pack=esp_cpu_get_cycle_count()-t;
                rx_mc6_result result={0};size_t bytes=0;
                memset(out,0x6d,5370);
                t=esp_cpu_get_cycle_count();
                bool valid=packed_ok && rx_mc6_finish(bits,out,5370,&bytes,&result,RX_MC6_BCH_BYTE);
                uint32_t finish=esp_cpu_get_cycle_count()-t;
                bool belief_equal=true;
                if(trial || arm)belief_equal=!memcmp(snapshot.raw+16,v,129600);
                else memcpy(snapshot.raw+16,v,129600);
                bool exact=stage && packed_ok && valid && bytes==5370 && !memcmp(bits,word.raw+16,8100) &&
                           !memcmp(out,contents,5370) && !result.unsatisfied && result.bch_ok && result.header_ok &&
                           esp_rom_crc32_le(0,bits,8100)==expected[c];
                bool finished=true;
                if(arm) {
                    rx_mc6_first saved=f;
                    finished=!rx_mc6_first_step_scalar(&f,1) && !rx_mc6_first_step(&f,2745) && !memcmp(&saved,&f,sizeof f);
                }
                bool guards=intact(belief)&&intact(snapshot)&&intact(channel)&&intact(packed)&&intact(payload)&&
                            intact(truth)&&intact(word)&&intact(encoded)&&(arm || intact(checks));
                bool unchanged=immutable==esp_rom_crc32_le(0,(const uint8_t *)llr,64800);
                ok=stage&&tiles==2745&&calls==43&&belief_equal&&exact&&finished&&guards&&unchanged;
                printf("@{\"cmd\":\"mc6_first_native\",\"case\":%u,\"trial\":%u,\"arm\":%u,\"flipped_bits\":%u,"
                       "\"forced_one_sweep\":true,\"PSRAM_IO\":true,\"belief_bytes\":129600,\"check_bytes\":%u,"
                       "\"tiles\":%u,\"calls\":%u,\"init_cycles\":%" PRIu32 ",\"step_cycles\":%" PRIu64 ","
                       "\"pack_cycles\":%" PRIu32 ",\"finish_cycles\":%" PRIu32 ",\"work_elapsed_cycles\":%" PRIu64 ","
                       "\"wait_us\":%" PRId64 ",\"word_crc32\":%" PRIu32 ",\"belief_equal\":%s,\"exact\":%s,"
                       "\"finished_gate\":%s,\"guards\":%s,\"immutable\":%s,\"passed\":%s}\n",
                       c,trial,arm,flips[c],arm?0U:86400U,tiles,calls,init,steps,pack,finish,
                       (uint64_t)init+steps+pack+finish,wait,esp_rom_crc32_le(0,bits,8100),yes(belief_equal),yes(exact),
                       yes(finished),yes(guards),yes(unchanged),yes(ok));
                heap_caps_free(checks.raw);
            }
    }
done:
    heap_caps_free(belief.raw);heap_caps_free(snapshot.raw);heap_caps_free(channel.raw);heap_caps_free(packed.raw);
    heap_caps_free(payload.raw);heap_caps_free(truth.raw);heap_caps_free(word.raw);heap_caps_free(encoded.raw);
    return ok;
}
