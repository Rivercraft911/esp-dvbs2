"""Execute the production handoff and radio worker with injected vendor failures.

No device access. These tests establish lifetime/cleanup, not undocumented RF semantics.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RADIO = ROOT / 'firmware/components/radio_s3'


def function(source, name):
    start = source.index('static ', source.rfind('\n}', 0, source.index(name + '(')) + 2)
    brace = source.index('{', source.index(name + '(', start))
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


NATIVE = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define CONFIG_PM_ENABLE 0
#define IRAM_ATTR
#define portMAX_DELAY UINT32_MAX
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define PHY_MODEM_WIFI 1
#define PHY_MODEM_BT 2
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_NULL 0
#define WIFI_PS_NONE 0
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define INTERRUPT_CORE0_MAC_INTR_MAP_REG 0
#define INTERRUPT_CORE1_MAC_INTR_MAP_REG 16
#define XCHAL_INTTYPE_MASK_EXTERN_EDGE 0x50400400U
#define XCHAL_INTTYPE_MASK_EXTERN_LEVEL 0x8fbe333fU
#define XCHAL_INTTYPE_MASK_NMI 0x4000U
#define DAC_CTRL 32
#define ADC_CTRL 36
#define BANK_USAGE 40
#define FE_CTRL 44
#define TONE_CTRL 48
#define pdMS_TO_TICKS(n) (n)
extern uint32_t regs[20];
static inline unsigned reg_index(uint32_t r) {
    if (r==0x60033d64U) r=DAC_CTRL;
    if (r==0x600c101cU) r=BANK_USAGE;
    if (r==0x60006000U) r=FE_CTRL;
    return r/4;
}
#define REG_READ(r) regs[reg_index(r)]
#define REG_WRITE(r,v) (regs[reg_index(r)] = (v))
#define REG_CLR_BIT(r,v) (regs[reg_index(r)] &= ~(v))
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
int esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
int esp_timer_start_periodic(esp_timer_handle_t, uint64_t);
int esp_timer_stop_blocking(esp_timer_handle_t, uint32_t);
int esp_timer_delete(esp_timer_handle_t);
typedef int wifi_mode_t;
typedef struct { int nvs_enable; } wifi_init_config_t;
unsigned phy_get_modem_flag(void);
void esp_wifi_bt_power_domain_on(void);
void esp_wifi_bt_power_domain_off(void);
void esp_phy_modem_init(void);
void esp_phy_modem_deinit(void);
void esp_phy_enable(unsigned);
void esp_phy_disable(unsigned);
int esp_cpu_get_core_id(void);
uint32_t esp_cpu_intr_get_enabled_mask(void);
unsigned heap_caps_get_free_size(unsigned);
unsigned heap_caps_get_largest_free_block(unsigned);
unsigned heap_caps_get_minimum_free_size(unsigned);
int esp_wifi_get_mode(wifi_mode_t *);
int esp_ipc_call_blocking(unsigned, void (*)(void *), void *);
'''

MODEL = r'''
#include "native.h"
#include "physolo.h"
uint32_t regs[20];
static unsigned users, domains, backups, core, calls, fail_at;
static bool initialized, started, expected_solo, expected_pace, played;
static unsigned calibration_begins, calibration_ends, calibration_restores;
static int64_t now;
int64_t esp_timer_get_time(void) { return ++now; }
static int stream_error;
static unsigned selection_calls;
static int payload_fill(uint32_t *d, uint64_t at, size_t n, void *ctx) {
    (void)d; (void)at; (void)n; (void)ctx; return ESP_OK;
}
static int selected_fill(uint32_t *d, uint64_t at, size_t n, void *ctx) {
    return payload_fill(d,at,n,ctx);
}
static radio_s3_fill_fn select_payload(radio_s3_fill_fn original) {
    assert(original==payload_fill && !selection_calls++);
    assert(!REG_READ(DAC_CTRL) && !(REG_READ(FE_CTRL)&2));
    return selected_fill;
}
static int vendor_call(void) { return ++calls == fail_at ? ESP_FAIL : ESP_OK; }
unsigned phy_get_modem_flag(void) { return users; }
void esp_wifi_bt_power_domain_on(void) { domains++; }
void esp_wifi_bt_power_domain_off(void) { assert(domains); domains--; }
void esp_phy_modem_init(void) { backups++; }
void esp_phy_modem_deinit(void) { assert(backups); backups--; }
void esp_phy_enable(unsigned m) { assert(!(users&m)); users |= m; }
void esp_phy_disable(unsigned m) { assert(users&m); users &= ~m; }
int esp_cpu_get_core_id(void) { return core; }
uint32_t esp_cpu_intr_get_enabled_mask(void) { return (1U<<0) | (1U<<6) | (1U<<16); }
unsigned heap_caps_get_free_size(unsigned c) { (void)c; return initialized ? 20000 : 35000; }
unsigned heap_caps_get_largest_free_block(unsigned c) { return heap_caps_get_free_size(c); }
unsigned heap_caps_get_minimum_free_size(unsigned c) { (void)c; return 19000; }
int esp_wifi_get_mode(wifi_mode_t *m) { *m=0; return initialized ? ESP_OK : ESP_ERR_WIFI_NOT_INIT; }
int esp_ipc_call_blocking(unsigned cpu, void (*fn)(void *), void *arg) {
    unsigned previous=core; core=cpu; fn(arg); core=previous; return ESP_OK;
}
static int esp_wifi_init(const wifi_init_config_t *c) {
    assert(!c->nvs_enable && !initialized); int e=vendor_call();
    if (!e) { initialized=true; esp_wifi_bt_power_domain_on(); esp_phy_modem_init(); }
    return e;
}
static int esp_wifi_set_storage(int n) { (void)n; return vendor_call(); }
static int esp_wifi_set_mode(int n) { (void)n; return vendor_call(); }
static int esp_wifi_start(void) {
    int e=vendor_call(); if (!e) { started=true; users |= PHY_MODEM_WIFI; } return e;
}
static int esp_wifi_set_ps(int n) { (void)n; return vendor_call(); }
static int esp_wifi_set_promiscuous(bool n) { (void)n; return vendor_call(); }
static int esp_wifi_set_channel(int n, int m) { (void)n; (void)m; return vendor_call(); }
static int esp_wifi_stop(void) {
    int e=vendor_call(); if (!e) { started=false; users &= ~PHY_MODEM_WIFI; } return e;
}
static int esp_wifi_deinit(void) {
    assert(initialized && !started); int e=vendor_call();
    if (!e) { initialized=false; esp_phy_modem_deinit(); esp_wifi_bt_power_domain_off(); }
    return e;
}
typedef struct {
    radio_s3_stats_t *stats;
    radio_s3_options_t options;
    radio_s3_fill_fn *payload_fill;
    radio_s3_startup_t *startup;
    bool wifi_pace, probe;
    struct { uint32_t rate, lead, step; } geo;
    uint64_t samples;
    uint32_t freq_mhz;
    unsigned gain;
    void *done;
    esp_err_t error;
} run_t;
static int esp_clk_cpu_freq(void) { return 240000000; }
static void heap_now(uint32_t *f, uint32_t *l) { *f=heap_caps_get_free_size(1); *l=*f; }
static int watch_begin(run_t *r, bool *i, bool *w) { (void)r; *i=*w=true; return ESP_OK; }
static void watch_end(bool i, bool w) { assert(i==w); }
static void force_txon_mode(unsigned on, unsigned m, unsigned o) {
    (void)m; (void)o; if (!on) REG_CLR_BIT(FE_CTRL, 2U);
}
static void rom_pbus_xpd_tx_off(void) { REG_CLR_BIT(FE_CTRL, 2U); }
static void rom_pbus_xpd_rx_on(unsigned n) { assert(!n); }
static void direct_lo(void) {}
static void set_chanfreq(unsigned f, unsigned b) { assert(f==2412 && !b); }
static void phy_set_freq(unsigned f, unsigned o) { assert(f==2412 && !o); }
static void tune(uint32_t mhz) { phy_set_freq(mhz, 0); }
static void vTaskDelay(unsigned n) { assert(n==3); }
static unsigned cw_channel(unsigned f) { (void)f; return 1; }
static unsigned cw_backoff(unsigned g) { (void)g; return 0; }
static void esp_phy_wifi_tx_tone(unsigned on, unsigned c, unsigned g) { assert(!on); (void)c; (void)g; }
bool radio_filter_finish(radio_filter_stats_t *s) { s->retained=true; return true; }
esp_err_t radio_calibration_begin(uint32_t mhz,unsigned gain,uint32_t rate,
                                  radio_calibration_stats_t *stats) {
    (void)mhz; (void)gain; (void)rate; (void)stats;
    assert(!initialized && !started && !REG_READ(DAC_CTRL));
    ++calibration_begins; return ESP_OK;
}
bool radio_calibration_restore(void) {
    assert(!REG_READ(DAC_CTRL));
    ++calibration_restores; return true;
}
void radio_calibration_end(radio_calibration_stats_t *stats) {
    (void)stats; assert(!REG_READ(DAC_CTRL)); ++calibration_ends;
}
static int play(run_t *r, bool pace) {
    assert(pace==expected_pace && !played);
    assert(initialized==!expected_solo && started==!expected_solo);
    assert(users==(expected_solo ? PHY_MODEM_BT : PHY_MODEM_WIFI));
    if (expected_solo) assert(r->stats->session.phy_users==users);
    if (!pace) {
        REG_WRITE(DAC_CTRL, (1U<<31)|(1U<<19)); REG_WRITE(BANK_USAGE,4);
    }
    if (!pace) {
        REG_WRITE(FE_CTRL,2);
        r->stats->filter.applied=true;
    }
    if (r->startup) {
        r->startup->count=1;
        r->startup->first_sample=123;
        r->startup->bank[0].cycles=456;
    }
    played=true; return stream_error;
}
static int paced(run_t *r) { return play(r,true); }
static int stream(run_t *r) { return play(r,false); }
static void xSemaphoreGive(void *s) { (void)s; }
static void vTaskDelete(void *t) { assert(!t); }
static bool ready_failure;
int radio_s3_stream_ready(void) {
    assert(initialized==!expected_solo && started==!expected_solo);
    assert(users==(expected_solo ? PHY_MODEM_BT : PHY_MODEM_WIFI));
    assert(!REG_READ(DAC_CTRL) && !(REG_READ(FE_CTRL)&2));
    return ready_failure ? ESP_FAIL : ESP_OK;
}
'''

MAIN = r'''
int main(void) {
    for (unsigned solo=0; solo<2; ++solo) for (unsigned pace=0; pace<2; ++pace)
    for (unsigned failure=0; failure<11; ++failure) for (unsigned stream_fail=0; stream_fail<2; ++stream_fail)
    for (unsigned ready_fail=0; ready_fail<2; ++ready_fail)
    for (unsigned pick=0; pick<2; ++pick) {
        if (!solo && failure>7) continue;
        selection_calls=0;
        calibration_begins=calibration_ends=calibration_restores=0;
        users=domains=backups=calls=0; core=1; fail_at=failure;
        initialized=started=played=false; memset(regs,0,sizeof regs);
        for (unsigned i=0;i<8;++i) regs[i]=6;
        expected_solo=solo; expected_pace=pace; stream_error=stream_fail ? ESP_FAIL : ESP_OK;
        ready_failure=ready_fail;
        radio_s3_stats_t stats={0};
        run_t run={.stats=&stats, .options={.phy_solo=solo}, .wifi_pace=pace,
                   .probe=pace, .freq_mhz=2412, .geo={.rate=80000000, .lead=128, .step=15740}, .samples=2400000000ULL};
        radio_s3_fill_fn payload=payload_fill;
        run.payload_fill=&payload;
        run.options.select_fill=pick ? select_payload : NULL;
        radio_s3_startup_t trace={.count=99};
        run.options.startup=&trace;
        worker(&run);
        assert(calibration_begins==!pace && calibration_ends==!pace);
        assert(calibration_restores==(played && !pace));
        assert(!run.startup);
        assert(trace.count==(pace ? (played ? 1u : 0u) : 99u));
        if (pace && played) assert(trace.first_sample==123 && trace.bank[0].cycles==456);
        assert(selection_calls==(pick && played));
        assert(payload==(pick && played ? selected_fill : payload_fill));
        assert(!initialized && !started && !users && !domains && !backups);
        assert(!REG_READ(DAC_CTRL) && !REG_READ(BANK_USAGE) && !(REG_READ(FE_CTRL)&2));
        if (solo) assert(!stats.after.wifi_initialized && !stats.after.phy_users);
        if (!failure && !ready_fail) assert(played && run.error==stream_error);
        if (ready_fail) assert(!played);
        if (played && !pace) assert(stats.filter.retained);
        if (pace) assert(!REG_READ(ADC_CTRL) && !REG_READ(TONE_CTRL));
    }
    users=PHY_MODEM_BT; calls=0; radio_s3_stats_t stats={0};
    run_t run={.stats=&stats, .options={.phy_solo=true}};
    worker(&run); assert(run.error==ESP_ERR_INVALID_STATE && !calls && users==PHY_MODEM_BT);
    users=0;
    for (unsigned i=0;i<8;++i) regs[i]=6;
    regs[0]=0; regs[1]=14; regs[2]=16;
    assert(!physolo_snapshot(&stats.session));
    assert(stats.session.wifi_irq_routes[0]==3 && !stats.session.wifi_irq_routes[1]);
    regs[4]=0; regs[5]=14;
    assert(!physolo_snapshot(&stats.session) && stats.session.wifi_irq_routes[1]==3);
    puts("PHY solo: repeated keyed/paced lifetimes, ten vendor failures, admission/stream aborts, existing client and both-core IRQ routes pass");
}
'''


TRACE_MODEL = r'''
#include "native.h"
#include "radio_s3.h"
#define EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG 0
static uint32_t clock_now;
uint32_t regs[20];
static uint32_t storage[2][RADIO_S3_BANK_WORDS];
static unsigned callbacks;
static bool abort_fill;
static uint32_t cycles(void) { return ++clock_now; }
uint32_t *radio_s3_bank(unsigned n) { return storage[n]; }
typedef struct {
    radio_s3_fill_fn fill;
    void *ctx;
    radio_s3_stats_t *stats;
    radio_s3_startup_t *startup;
} run_t;
static esp_err_t payload(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    assert(ctx == storage && count && dst >= storage[0] && dst < storage[1]+RADIO_S3_BANK_WORDS);
    (void)first;
    ++callbacks; clock_now += 100; regs[0] += 7;
    return abort_fill ? ESP_FAIL : ESP_OK;
}
'''

TRACE_MAIN = r'''
int main(void)
{
    radio_s3_stats_t stats={.id_samples=417600000};
    radio_s3_startup_t trace={.first_sample=417597812};
    run_t run={.fill=payload, .ctx=storage, .stats=&stats, .startup=&trace};
    assert(!fill_bank(&run,0,0) && callbacks==1 && !trace.count);
    // The first intersecting bank is split at physical wrap. Unsigned miss
    // deltas must also survive counter wrap; this is one complete bank record.
    regs[0]=UINT32_MAX-3;
    assert(!fill_bank(&run,1,trace.first_sample) && callbacks==3);
    assert(trace.count==1 && trace.bank[0].cycles==201 && trace.bank[0].psram_miss==14);
    assert(trace.bank[0].trace_cycles && stats.max_fill_cycles==201);
    assert(stats.max_fill_sample==trace.first_sample);
    for (unsigned i=1;i<32;++i)
        assert(!fill_bank(&run,i&1,trace.first_sample+15740*i));
    assert(trace.count==32);
    unsigned old=trace.bank[31].cycles;
    assert(!fill_bank(&run,0,trace.first_sample+15740*32) && trace.count==32 && trace.bank[31].cycles==old);
    // Abort after the first callback; retain the failing bank's evidence.
    trace.count=0; callbacks=0; abort_fill=true;
    assert(fill_bank(&run,0,trace.first_sample)==ESP_FAIL && callbacks==1 && trace.count==1);
    assert(trace.bank[0].cycles==101 && trace.bank[0].psram_miss==7);
    run.startup=NULL; callbacks=0;
    assert(fill_bank(&run,0,0)==ESP_FAIL && callbacks==1 && trace.count==1);
    puts("startup trace: ID edge, split bank, counter wrap, 32-bank bound, disabled trace and abort evidence pass");
}
'''

with tempfile.TemporaryDirectory() as folder:
    tmp = Path(folder)
    (tmp / 'native.h').write_text(NATIVE)
    for name in ('esp_err.h', 'esp_cpu.h', 'esp_heap_caps.h', 'esp_ipc.h', 'esp_phy_init.h',
                 'esp_private/phy.h', 'esp_wifi.h', 'soc/interrupt_core0_reg.h',
                 'soc/interrupt_core1_reg.h', 'soc/soc.h', 'sdkconfig.h', 'xtensa/config/core-isa.h',
                 'esp_timer.h', 'esp_attr.h', 'freertos/FreeRTOS.h'):
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "native.h"\n')
    worker = function((RADIO / 'radio_s3.c').read_text(), 'worker')
    (tmp / 'test.c').write_text(MODEL + worker + MAIN)
    exe = tmp / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(tmp), '-I'+str(RADIO), '-I'+str(RADIO / 'include'),
                    str(tmp / 'test.c'), str(RADIO / 'physolo.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    bank = function((RADIO / 'radio_s3.c').read_text(), 'fill_bank')
    bank = bank.replace('__asm__ volatile ("memw" ::: "memory");', '')
    (tmp / 'trace.c').write_text(TRACE_MODEL + bank + TRACE_MAIN)
    trace_exe = tmp / 'trace'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(tmp), '-I'+str(RADIO / 'include'),
                    str(tmp / 'trace.c'), '-o', str(trace_exe)], check=True)
    subprocess.run([str(trace_exe)], check=True)
