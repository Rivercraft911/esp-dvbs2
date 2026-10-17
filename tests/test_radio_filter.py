"""Compile the real filter control and CW bring-up against register/ROM fakes. No radio."""
import json
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'firmware/components/radio_s3'
source = (COMPONENT / 'radio_s3.c').read_text()
stream = source[source.index('static esp_err_t IRAM_ATTR __attribute__((noinline)) stream('):
                source.index('/* Radio off: the fills of stream()')]

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio_s3.h"
#include "soc/soc.h"
#define IRAM_ATTR
#define DAC_CTRL 0x60033d64U
#define BANK_USAGE 0x600c101cU
#define FE_CTRL 0x60006000U
#define TONE_CTRL 0x60006040U
#define DAC_RUN (1U << 31)
#define DAC_LOOP (1U << 19)
static uint32_t regs[8];
static const uint32_t addresses[] = {0x60006100,0x6002600c,0x60033400,0x6001d000,
                                    0x6001cc0c,BANK_USAGE,FE_CTRL,TONE_CTRL};
static uint8_t caps[32];
static unsigned reads, rom_calls, ana_calls, phase, dac_writes;
static bool bad_readback;
uint32_t mock_read(uint32_t addr) {
    ++reads;
    for (unsigned i=0;i<8;++i) if (addr==addresses[i]) return regs[i];
    assert(0); return 0;
}
void mock_write(uint32_t addr, uint32_t value) {
    if (addr==DAC_CTRL) { assert(phase==4); assert(value&DAC_RUN); ++dac_writes; return; }
    for (unsigned i=0;i<8;++i) if (addr==addresses[i]) { regs[i]=value; return; }
    assert(0);
}
uint8_t regi2c_ctrl_read_reg(uint8_t block,uint8_t host,uint8_t reg) {
    assert(block==0x67 && host==0); ++reads; return caps[reg];
}
void rom_bb_bss_cbw40_ana(unsigned wide) {
    ++ana_calls;
    regs[0]=(regs[0]&~0x3f0000U)|(wide?0x120000U:0);
}
void rom_bb_bss_cbw40(unsigned mode) {
    assert(phase==0 || phase==3); // after CW stop and PLL retune, before DAC
    phase=4; ++rom_calls; assert(mode==0 || mode==2);
    // Independent ROM trace expectations, preserving unrelated fields.
    regs[2]=(regs[2]&~3U)|(mode==2?3U:0);
    regs[1]=(regs[1]&~12U)|(mode==2?4U:0);
    if(mode==2) { regs[3]&=~64U; regs[4]&=~1U; }
    rom_bb_bss_cbw40_ana(mode==2);
    if(bad_readback) regs[0]^=1U<<17;
}
typedef struct { uint32_t step,lead,rate_bit; } geometry_t;
typedef struct { radio_s3_stats_t *stats; radio_s3_chain_t chain;
                 unsigned freq_mhz,gain; geometry_t geo; } run_t;
static int fill_bank(run_t *r,unsigned bank,uint64_t first) { (void)r;(void)bank;(void)first;return 0; }
static const uint8_t gain_codes[18]={0};
static unsigned gain_baseband(unsigned gain) { return gain; }
static uint32_t cw_channel(uint32_t freq) { (void)freq; return 3; }
static uint32_t cw_backoff(unsigned gain) { return gain; }
static void esp_phy_cbw40m_en(bool en) { (void)en; }
static void esp_phy_wifi_tx_tone(unsigned start,unsigned chan,unsigned backoff) {
    (void)chan;(void)backoff;assert(start==1 && phase==0);phase=1;
}
static void stop_tx_tone(unsigned en) { assert(en==1 && phase==1);phase=2; }
static void phy_set_freq(unsigned mhz,int khz) { assert(mhz==2420 && khz==0 && phase==2);phase=3; }
static void direct_lo(void) { assert(phase==3); }
static void force_tx_gain(unsigned a,unsigned b,unsigned c) { (void)a;(void)b;(void)c; }
static void force_txon_mode(unsigned a,unsigned b,unsigned c) { (void)a;(void)b;(void)c; }
static void rom_pbus_workmode(void) { assert(0); }
static void rom_pbus_xpd_rx_on(unsigned en) { (void)en;assert(0); }
static void rom_pbus_xpd_tx_on(unsigned en) { (void)en;assert(0); }
static unsigned portSET_INTERRUPT_MASK_FROM_ISR(void) { return 0; }
static void portCLEAR_INTERRUPT_MASK_FROM_ISR(unsigned irq) { (void)irq; }
static unsigned cycles(void) { return 1; }
static int schedule(run_t *r,unsigned epoch,unsigned owner,bool keyed) {
    (void)r;(void)epoch;(void)owner;assert(keyed && dac_writes==2);return 0;
}
''' + stream + r'''
int main(void) {
    assert(radio_filter_get()==RADIO_FILTER_AUTO);
    assert(!radio_filter_set("wide extra") && !radio_filter_set("2") && reads==0 && rom_calls==0);
    const char *names[]={"auto","narrow","wide","analog"};
    for(unsigned request=0;request<4;++request) {
        assert(radio_filter_set(names[request]));
        for(unsigned b=0;b<5;++b) {
            const unsigned bandwidths[]={1350000,20000000,20000001,36000000,40000000};
            for(unsigned chain=1;chain<4;++chain) {
                memset(regs,0,sizeof regs); regs[0]=0xffc0ffff;
                for(unsigned j=0;j<32;++j) caps[j]=(uint8_t)(0xc0U+j);
                phase=0; dac_writes=0; bad_readback=false;
                radio_s3_stats_t stats={0};
                stats.filter.requested=radio_filter_get(); stats.filter.occupied_hz=bandwidths[b];
                run_t run={.stats=&stats,.chain=chain,.freq_mhz=2420,.geo={15740,128,1U<<15}};
                assert(stream(&run)==0 && dac_writes==2);
                unsigned selected=request ? request : bandwidths[b]>20000000?2:1;
                assert(stats.filter.selected==selected && stats.filter.applied && stats.filter.matches);
                assert((stats.filter.active.analog&0x3f0000U)==(selected==1?0:0x120000U));
                assert((stats.filter.active.channel&3U)==(selected==2?3:0));
                assert(stats.filter.before.analog==0xffc0ffff);
                assert(stats.filter.active.caps[4]==0xcc && stats.filter.active.caps[7]==0xcf);
                assert(radio_filter_finish(&stats.filter));
                for(unsigned field=0;field<5;++field) {
                    const unsigned bits[]={0x20000,4,1,64,1};
                    regs[field]^=bits[field];
                    assert(radio_filter_finish(&stats.filter)==(selected!=2 && field>=3));
                    regs[field]^=bits[field];
                }
                caps[12]^=1; assert(!radio_filter_finish(&stats.filter)); caps[12]^=1;
                assert(radio_filter_finish(&stats.filter));
                printf("@{"); printf("\"case\":%u",b); radio_filter_print(&stats.filter); printf("}\n");
            }
        }
    }
    radio_s3_stats_t stats={0}; stats.filter.requested=RADIO_FILTER_WIDE;
    run_t run={.stats=&stats,.chain=RADIO_S3_CHAIN_CW_GAIN,.freq_mhz=2420,.geo={15740,128,0}};
    phase=0; dac_writes=0; bad_readback=true;
    assert(stream(&run)==ESP_ERR_INVALID_STATE && dac_writes==0 && !stats.filter.matches);
    assert(ana_calls>=rom_calls);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    (tmp/'soc').mkdir(); (tmp/'esp_private').mkdir()
    (tmp/'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_STATE 2\n')
    (tmp/'soc/soc.h').write_text('#pragma once\n#include <stdint.h>\nuint32_t mock_read(uint32_t);\nvoid mock_write(uint32_t,uint32_t);\n#define REG_READ(a) mock_read(a)\n#define REG_WRITE(a,v) mock_write(a,v)\n#define REG_CLR_BIT(a,v) mock_write(a,mock_read(a)&~(v))\n')
    (tmp/'esp_private/regi2c_ctrl.h').write_text('#include <stdint.h>\nuint8_t regi2c_ctrl_read_reg(uint8_t,uint8_t,uint8_t);\n')
    (tmp/'filter.c').write_text(harness)
    exe = tmp/'filter'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(tmp),'-I',str(COMPONENT/'include'),
                    str(tmp/'filter.c'),str(COMPONENT/'radio_filter.c'),'-o',str(exe)],check=True)
    result = subprocess.run([str(exe)],capture_output=True,text=True,check=True)
    rows=[json.loads(line[1:]) for line in result.stdout.splitlines()]
    assert len(rows)==60 and all(r['filter']['retained'] for r in rows)
print('filter: auto boundary, overrides, CW/PLL/filter/DAC order, readback failures, retention and JSON pass (ROM/MMIO fakes, no RF)')
