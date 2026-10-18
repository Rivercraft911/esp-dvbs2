// Exercise the actual observer with synthetic nested interrupts and counter wraps.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define STREAM_JITTER_HOST
#define DRAM_ATTR
#define IRAM_ATTR
typedef void (*xt_handler)(void *);
static unsigned current_core, interrupt_mask;
static uint32_t clock_now, perf[8], matrix[8];
static unsigned Xthal_intlevel[32];
#define INTERRUPT_CORE0_MAC_INTR_MAP_REG 0
#define INTERRUPT_CORE1_MAC_INTR_MAP_REG 16
#define ETS_MAX_INTR_SOURCE 4
static const char *esp_isr_names[] = {"wifi","usb","tick","yield"};
#define REG_READ(r) matrix[(r)/4]
#define ERI_PERFMON_PGM 0
#define ERI_PERFMON_PMCTRL0 4
#define ERI_PERFMON_PM0 12
#define ERI_PERFMON_PM1 16
static uint32_t eri_read(unsigned r) { return perf[r/4]; }
static void eri_write(unsigned r, uint32_t x) { perf[r/4] = x; }
static unsigned esp_cpu_get_core_id(void) { return current_core; }
static uint32_t esp_cpu_get_cycle_count(void) { return clock_now; }
static uint32_t portSET_INTERRUPT_MASK_FROM_ISR(void)
{ unsigned old = interrupt_mask; interrupt_mask = 3; return old; }
static void portCLEAR_INTERRUPT_MASK_FROM_ISR(unsigned old) { interrupt_mask = old; }
static int xt_int_has_handler(unsigned n, unsigned core);
static xt_handler xt_set_interrupt_handler(int n, xt_handler handler, void *arg);
#define ESP_ERROR_CHECK(x) assert((x) == 0)
static int esp_ipc_call_blocking(unsigned core, void (*fn)(void *), void *arg)
{ unsigned saved = current_core; current_core = core; fn(arg); current_core = saved; return 0; }
#include "../firmware/main/stream_jitter.c"
dispatch_t _xt_interrupt_table[64];
static int xt_int_has_handler(unsigned n, unsigned core)
{ return _xt_interrupt_table[2*n+core].handler != NULL; }
static xt_handler xt_set_interrupt_handler(int n, xt_handler handler, void *arg)
{
    assert(interrupt_mask == 3);
    dispatch_t *p = &_xt_interrupt_table[2*n+current_core];
    xt_handler old = p->handler;
    p->handler = handler; p->arg = arg;
    return old;
}
static void fire(unsigned n)
{
    dispatch_t *p = &_xt_interrupt_table[2*n+current_core];
    p->handler(p->arg);
}
static void inner(void *arg) { assert(arg == &matrix[1]); clock_now += 20; }
static void outer(void *arg)
{
    assert(arg == &matrix[0]); clock_now += 10; fire(19); clock_now += 10;
}
static void ordinary(void *arg) { assert(arg == &matrix[4]); clock_now += 100; }

int main(void)
{
    Xthal_intlevel[5] = Xthal_intlevel[7] = 1;
    Xthal_intlevel[19] = 2;
    _xt_interrupt_table[2*5] = (dispatch_t){outer,&matrix[0]};
    _xt_interrupt_table[2*19] = (dispatch_t){inner,&matrix[1]};
    _xt_interrupt_table[2*7+1] = (dispatch_t){ordinary,&matrix[4]};
    matrix[0] = 5; matrix[1] = 19; matrix[4] = 7;
    dispatch_t saved[64]; memcpy(saved,_xt_interrupt_table,sizeof saved);
    for (unsigned i = 0; i < 8; ++i) perf[i] = 17+i;
    uint32_t saved_perf[8]; memcpy(saved_perf,perf,sizeof perf);
    stream_jitter_enable(1);
    current_core = 1; radio_s3_profile_start(); current_core = 0;
    fire(5);
    assert(cpu[0].irq[0].cycles == 20 && cpu[0].irq[0].max_cycles == 40);
    assert(cpu[0].irq[1].cycles == 20 && cpu[0].cycles[0]+cpu[0].cycles[1] == 40);
    current_core = 1;
    const uint32_t elapsed[] = {31999,32000,35999,36000,40137,40138,43999,44000,47219,47220};
    for (unsigned i = 0; i < 10; ++i) {
        perf[ERI_PERFMON_PM0/4] = UINT32_MAX-10;
        perf[ERI_PERFMON_PM1/4] = UINT32_MAX-20;
        radio_s3_profile_bank_begin();
        // Two wakes in separate callback fragments of the same bank. The
        // following bank begins at zero, including one with no notification.
        if (i%3) {
            stream_jitter_notify(100+i);
            stream_jitter_notify(200+i);
        }
        for (unsigned n = 0; n < i%5; ++n) fire(7);
        perf[ERI_PERFMON_PM0/4] += 50;
        perf[ERI_PERFMON_PM1/4] += 60;
        radio_s3_profile_bank_end(elapsed[i],i&1,15740*i);
        if (i == 8) assert(worst.notify_calls == 2 && worst.notify_cycles == 316 && worst.notify_max_cycles == 208);
    }
    radio_s3_profile_stop();
    assert(!memcmp(saved,_xt_interrupt_table,sizeof saved));
    assert(!memcmp(saved_perf,perf,sizeof perf));
    assert(worst.cycles == 47220 && worst.irqs == 4 && worst.irq_cycles == 400);
    assert(worst.data_busy == 50 && worst.high_irq_entries == 60 && worst.vectors == (1u<<7));
    assert(worst.notify_calls == 0 && worst.notify_cycles == 0);
    assert(buckets[0][0].calls == 1 && buckets[0][0].histogram[0] == 1);
    assert(buckets[1][0].calls == 1 && buckets[1][0].histogram[3] == 1);
    printf("{\"test\":\"jitter\""); stream_jitter_print(); puts("}");
    fire(7); // Original argument and handler remain usable after teardown.
    current_core = 0;
    for (unsigned n = 0; n < 17; ++n) {
        Xthal_intlevel[n] = 1;
        _xt_interrupt_table[2*n] = (dispatch_t){ordinary,&matrix[4]};
    }
    memcpy(saved,_xt_interrupt_table,sizeof saved);
    stream_jitter_enable(1); stream_jitter_core_start();
    assert(cpu[0].used == 16 && cpu[0].omitted > 0);
    stream_jitter_core_stop();
    assert(!memcmp(saved,_xt_interrupt_table,sizeof saved));
    stream_jitter_enable(0); stream_jitter_core_start(); stream_jitter_core_stop();
    stream_jitter_notify(999);
    assert(mark.notify_calls == 0);
    assert(!memcmp(saved,_xt_interrupt_table,sizeof saved));
    return 0;
}
