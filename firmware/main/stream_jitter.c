#include "stream_jitter.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(__XTENSA__) || defined(STREAM_JITTER_HOST)
#ifndef STREAM_JITTER_HOST
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/interrupt_core0_reg.h"
#include "soc/interrupt_core1_reg.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "eri.h"
#include "xtensa-debug-module.h"
#include "xtensa_api.h"
#endif

// IDF v6.1 xtensa/xtensa_intr.c: the C dispatch table is [vector*2+core].
// Install/restore on the owning CPU with levels 1..3 masked. No ISR allocation,
// logging or scheduler calls, and no change to the interrupt matrix or priority.
typedef struct { xt_handler handler; void *arg; } dispatch_t;
extern dispatch_t _xt_interrupt_table[];
typedef struct {
    dispatch_t saved;
    uint32_t calls, max_cycles;
    uint64_t cycles;
    unsigned vector, level;
} irq_t;
typedef struct {
    irq_t irq[16];                 // report incomplete rather than silently omit extra handlers
    uint32_t calls[3], cycles[3], mask[3];
    unsigned used, omitted;
    int installed;
} cpu_t;
static DRAM_ATTR cpu_t cpu[2];
static uint8_t routes[2][ETS_MAX_INTR_SOURCE];
static int enabled;
static uint32_t perf_saved[5];
typedef struct { uint32_t calls, cycles, data_busy, high_irq_entries; } mark_t;
static mark_t mark;
typedef struct {
    uint32_t calls, max_cycles, irq_cycles, data_busy, high_irq_entries;
    uint32_t histogram[6];
} bucket_t;
static bucket_t buckets[2][5];
static struct {
    uint32_t cycles, irqs, irq_cycles, vectors, data_busy, high_irq_entries, bank;
    uint64_t first;
} worst;

static void IRAM_ATTR counted_irq(void *arg)
{
    irq_t *p = arg;
    cpu_t *c = &cpu[esp_cpu_get_core_id()];
    unsigned level = p->level-1;
    uint32_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t nested = c->cycles[0]+c->cycles[1]+c->cycles[2];
    uint32_t before = esp_cpu_get_cycle_count();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
    p->saved.handler(p->saved.arg);
    mask = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t elapsed = esp_cpu_get_cycle_count()-before;
    // A higher level can nest. Count its handler only once in the aggregate.
    uint32_t own = elapsed-(c->cycles[0]+c->cycles[1]+c->cycles[2]-nested);
    ++p->calls;
    p->cycles += own;
    if (elapsed > p->max_cycles) p->max_cycles = elapsed;
    ++c->calls[level];
    c->cycles[level] += own;
    c->mask[level] |= 1u<<p->vector;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

void stream_jitter_enable(int on)
{
    enabled = on;
    memset(cpu,0,sizeof cpu);
    memset(buckets,0,sizeof buckets);
    memset(&worst,0,sizeof worst);
}

void stream_jitter_core_start(void)
{
    if (!enabled) return;
    unsigned core = esp_cpu_get_core_id();
    cpu_t *c = &cpu[core];
    uint32_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    for (unsigned n = 0; n < 32; ++n) {
        unsigned level = Xthal_intlevel[n];
        if (!level || level > 3 || !xt_int_has_handler(n,core)) continue;
        if (c->used == 16) { ++c->omitted; continue; }
        irq_t *p = &c->irq[c->used++];
        p->saved = _xt_interrupt_table[2*n+core];
        p->vector = n;
        p->level = level;
        xt_set_interrupt_handler(n,counted_irq,p);
    }
    c->installed = 1;
    const uint32_t base = core ? INTERRUPT_CORE1_MAC_INTR_MAP_REG : INTERRUPT_CORE0_MAC_INTR_MAP_REG;
    for (unsigned n = 0; n < ETS_MAX_INTR_SOURCE; ++n)
        routes[core][n] = REG_READ(base+4*n)&31;
    if (core == 1) {
        perf_saved[0] = eri_read(ERI_PERFMON_PGM);
        for (unsigned n = 0; n < 2; ++n) {
            perf_saved[1+2*n] = eri_read(ERI_PERFMON_PMCTRL0+4*n);
            perf_saved[2+2*n] = eri_read(ERI_PERFMON_PM0+4*n);
        }
        // IDF xt_perf_consts.h: D_STALL busy/bank-conflict, EXR interrupts above level 1.
        // Both include ISR windows; neither is an exclusive attribution to core 0.
        eri_write(ERI_PERFMON_PGM,0);
        eri_write(ERI_PERFMON_PMCTRL0,(0x110u<<16)|(3u<<8)|(15u<<4));
        eri_write(ERI_PERFMON_PMCTRL0+4,(4u<<16)|(5u<<8)|(15u<<4));
        eri_write(ERI_PERFMON_PGM,1);
    }
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

void stream_jitter_core_stop(void)
{
    cpu_t *c = &cpu[esp_cpu_get_core_id()];
    if (!c->installed) return;
    uint32_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    for (unsigned n = 0; n < c->used; ++n)
        xt_set_interrupt_handler(c->irq[n].vector,c->irq[n].saved.handler,c->irq[n].saved.arg);
    if (esp_cpu_get_core_id() == 1) {
        eri_write(ERI_PERFMON_PGM,0);
        for (unsigned n = 0; n < 2; ++n) {
            eri_write(ERI_PERFMON_PMCTRL0+4*n,perf_saved[1+2*n]);
            eri_write(ERI_PERFMON_PM0+4*n,perf_saved[2+2*n]);
        }
        eri_write(ERI_PERFMON_PGM,perf_saved[0]);
    }
    c->installed = 0;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

// The radio component's weak hooks run only in radio-off paced diagnostics.
static void start_other(void *arg) { (void)arg; stream_jitter_core_start(); }
static void stop_other(void *arg) { (void)arg; stream_jitter_core_stop(); }
void radio_s3_profile_start(void)
{
    if (!enabled) return;
    // Wi-Fi installs handlers during setup. Snapshot after it is up, on each
    // owning CPU. These IPC callbacks neither block nor call the scheduler.
    ESP_ERROR_CHECK(esp_ipc_call_blocking(0,start_other,NULL));
    stream_jitter_core_start();
}
void radio_s3_profile_stop(void)
{
    if (!enabled) return;
    stream_jitter_core_stop();
    ESP_ERROR_CHECK(esp_ipc_call_blocking(0,stop_other,NULL));
}

void IRAM_ATTR radio_s3_profile_bank_begin(void)
{
    if (!enabled) return;
    cpu_t *c = &cpu[1];
    uint32_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    mark.calls = c->calls[0]+c->calls[1]+c->calls[2];
    mark.cycles = c->cycles[0]+c->cycles[1]+c->cycles[2];
    c->mask[0] = c->mask[1] = c->mask[2] = 0;
    mark.data_busy = eri_read(ERI_PERFMON_PM0);
    mark.high_irq_entries = eri_read(ERI_PERFMON_PM1);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

void IRAM_ATTR radio_s3_profile_bank_end(uint32_t cycles, unsigned bank, uint64_t first)
{
    if (!enabled) return;
    cpu_t *c = &cpu[1];
    uint32_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t calls = c->calls[0]+c->calls[1]+c->calls[2]-mark.calls;
    uint32_t spent = c->cycles[0]+c->cycles[1]+c->cycles[2]-mark.cycles;
    uint32_t vectors = c->mask[0]|c->mask[1]|c->mask[2];
    uint32_t data = eri_read(ERI_PERFMON_PM0)-mark.data_busy;
    uint32_t high_irq = eri_read(ERI_PERFMON_PM1)-mark.high_irq_entries;
    bucket_t *b = &buckets[bank][calls < 4 ? calls : 4];
    ++b->calls;
    unsigned bin = cycles < 32000 ? 0 : cycles < 36000 ? 1 : cycles <= 40137 ? 2 :
                   cycles < 44000 ? 3 : cycles < 47220 ? 4 : 5;
    ++b->histogram[bin];
    if (cycles > b->max_cycles) {
        b->max_cycles = cycles;
        b->irq_cycles = spent;
        b->data_busy = data;
        b->high_irq_entries = high_irq;
    }
    if (cycles > worst.cycles) {
        worst.cycles = cycles; worst.irqs = calls; worst.irq_cycles = spent;
        worst.vectors = vectors; worst.data_busy = data; worst.high_irq_entries = high_irq;
        worst.bank = bank; worst.first = first;
    }
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

void stream_jitter_print(void)
{
    if (!enabled) return;
    printf(",\"jitter\":{\"complete\":%s,\"irq_time_scope\":\"C handlers levels 1..3; excludes vector entry/exit and scheduling\","
           "\"histogram_upper_cycles\":[31999,35999,40137,43999,47219,null],\"banks_by_irq_count\":[",
           cpu[0].omitted || cpu[1].omitted ? "false" : "true");
    for (unsigned n = 0; n < 10; ++n) {
        bucket_t *b = &buckets[n/5][n%5];
        printf("%s{\"bank\":%u,\"irqs\":%u,\"calls\":%u,\"max_cycles\":%u,\"irq_cycles_at_max\":%u,"
               "\"data_busy_at_max\":%u,\"high_irq_entries_at_max\":%u,\"histogram\":[",
               n ? "," : "",n/5,n%5,(unsigned)b->calls,(unsigned)b->max_cycles,(unsigned)b->irq_cycles,(unsigned)b->data_busy,(unsigned)b->high_irq_entries);
        for (unsigned j = 0; j < 6; ++j) printf("%s%u",j ? "," : "",(unsigned)b->histogram[j]);
        printf("]}");
    }
    printf("],\"worst_bank\":{\"cycles\":%u,\"irqs\":%u,\"irq_cycles\":%u,\"vectors\":%u,"
           "\"data_busy\":%u,\"high_irq_entries\":%u,\"bank\":%u,\"first\":%"PRIu64"},\"cores\":[",
           (unsigned)worst.cycles,(unsigned)worst.irqs,(unsigned)worst.irq_cycles,(unsigned)worst.vectors,(unsigned)worst.data_busy,
           (unsigned)worst.high_irq_entries,(unsigned)worst.bank,worst.first);
    for (unsigned core = 0; core < 2; ++core) {
        printf("%s[",core ? "," : "");
        unsigned printed = 0;
        for (unsigned index = 0; index < cpu[core].used; ++index) {
            irq_t *p = &cpu[core].irq[index];
            unsigned n = p->vector;
            printf("%s{\"vector\":%u,\"level\":%u,\"calls\":%u,\"cycles\":%"PRIu64",\"max_cycles\":%u,\"sources\":[",
                   printed++ ? "," : "",n,p->level,(unsigned)p->calls,p->cycles,(unsigned)p->max_cycles);
            unsigned sources = 0;
            for (unsigned source = 0; source < ETS_MAX_INTR_SOURCE; ++source) {
                if (routes[core][source] == n)
                    printf("%s\"%s\"",sources++ ? "," : "",esp_isr_names[source]);
            }
            printf("]}");
        }
        printf("]");
    }
    printf("]}");
}
#else
void stream_jitter_enable(int enabled) { (void)enabled; }
void stream_jitter_core_start(void) {}
void stream_jitter_core_stop(void) {}
void stream_jitter_print(void) {}
#endif
