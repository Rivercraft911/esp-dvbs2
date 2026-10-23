// Continuous dump-engine capture on core 1. Register facts: cyber-murmel/esp-sdr-zephyr
// 3214c16a7c9f136414cc5ae9d245ea290c98a6ce, doc/undocumented-registers-esp32s3.md
// (dump control 196-246, lanes 312-332, bank ownership 343-383, RX word format 1355-1365,
// vendor calls 1377-1386). The RF setup order and the sentinel edge search follow the
// finite receiver on exp/s3-receive (firmware/rx/main/capture.c, samples.c).
#include <string.h>
#include "capture.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_async_memcpy.h"
#include "esp_phy_init.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/phy.h"
#include "driver/gptimer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/syscon_reg.h"

#define DUMP_CTRL 0x60033d5cU
#define DUMP_INDEX 0x60033d60U
#define DAC_CTRL 0x60033d64U
#define DUMP_LANES 0x60033d90U
#define BANK_USAGE 0x600c101cU
#define AGC_FORCE 0x6001c02cU
#define FE_CTRL 0x60006000U
#define DUMP_RUN (1U << 31)
#define DUMP_CIRCULAR (1U << 17)
#define DUMP_16M (1U << 16)
#define RING_MASK (CAPTURE_RING - 1U)
#define SENTINEL 0xa5a5a5a5U
#define EDGE_GUARD 64U
#define PAIR_CYCLES 15U         /* 240 MHz / 16 MS/s */
#define CAPTURE_STACK 4096U     /* the consumer runs here; it needs well under 3 KB */

SOC_RESERVE_MEMORY_REGION(0x3fcb0000, 0x3fce0000, capture_banks);

extern void stop_tx_tone(unsigned int enable);
extern void force_txon_mode(uint8_t enable, uint8_t mode, uint8_t offset);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_pbus_xpd_rx_on(unsigned int enable);
extern void rom_set_rxclk_en(unsigned int enable);
extern void set_chanfreq(uint16_t mhz, uint8_t bandwidth);
extern void phy_set_freq(uint16_t mhz, int16_t khz);
extern void force_rx_gain(uint8_t force, uint8_t index, uint8_t bluetooth);

typedef struct {
    const capture_config_t *config;
    capture_fn fn;
    void *ctx;
    capture_stats_t *stats;
    uint32_t *bench;    /* membench results instead of a stream, or NULL */
    SemaphoreHandle_t done;
} job_t;

static inline uint32_t *bank(unsigned n)
{
    return (uint32_t *)(0x3fcb0000U + n * 65536U);
}

static void IRAM_ATTR clear(uint32_t *p)
{
    static DRAM_ATTR const uint32_t pattern[4] __attribute__((aligned(16))) =
        {SENTINEL, SENTINEL, SENTINEL, SENTINEL};
    const uint32_t *src = pattern;
    __asm__ volatile ("ee.vld.128.ip q0, %0, 0" : "+r"(src));
    for (uint32_t *end = p + CAPTURE_RING; p != end;)
        __asm__ volatile ("ee.vst.128.ip q0, %0, 16\n ee.vst.128.ip q0, %0, 16\n"
                          "ee.vst.128.ip q0, %0, 16\n ee.vst.128.ip q0, %0, 16" : "+r"(p) :: "memory");
    __asm__ volatile ("memw" ::: "memory");
}

/* The handoff index lies in [before, after]. Exactly one sentinel/data transition
 * in that bracket, widened by the guard, marks it. */
static bool IRAM_ATTR edge(const uint32_t *b, unsigned before, unsigned after, bool entering,
                           unsigned *index)
{
    unsigned width = (after - before) & RING_MASK, at = (before - EDGE_GUARD) & RING_MASK;
    if (width > EDGE_GUARD) return false;
    bool previous = b[(at - 1U) & RING_MASK] != SENTINEL;
    unsigned found = 0;
    for (unsigned n = 0; n <= width + 2U * EDGE_GUARD; ++n, at = (at + 1U) & RING_MASK) {
        bool present = b[at] != SENTINEL;
        if (present != previous) {
            if (present != entering) return false;
            *index = at;
            ++found;
        }
        previous = present;
    }
    return found == 1;
}

/* LO 5/6 divider, CKGEN 0x65:0 bit 4: below 2200 MHz the PLL runs at 6/5 of RF. */
static void select_lo(bool divided)
{
    regi2c_ctrl_write_reg_mask(0x65, 0, 0, 4, 4, divided);
}

/* Handoffs run in a timer interrupt on core 1, so the consumer may lag. The engine
 * takes a bank the task has finished; if there is none, it takes the newest bank the
 * task has not started, whose unit is then lost. The task sees that as a gap. */
#define TIMER_HZ 10000000U
#define EARLY_US 15U            /* alarm this far before a handoff, then spin */
#define UNIT_TICKS ((uint64_t)CAPTURE_UNIT * TIMER_HZ / CAPTURE_RATE)
#define EARLY_TICKS ((uint64_t)EARLY_US * (TIMER_HZ / 1000000U))
#define RECORDS 8U              /* handoffs the task may trail by */
typedef struct {
    gptimer_handle_t timer;
    uint32_t owner, current, max_late, max_spin, drops;
    uint16_t index, before[RECORDS], after[RECORDS];
    uint8_t bank[RECORDS], free[4], reowned[RECORDS], current_reowned;
    volatile uint8_t dropped[RECORDS];
    uint64_t pairs, target, at[RECORDS];
    volatile uint32_t posted, done, working, free_head, free_tail;
    const char *volatile fault;
    TaskHandle_t task;
} sched_t;

/* Sentinels after where a taken-back bank's next unit will end, so the end-edge search
 * works although the task never cleared it. Its start is the previous unit's end. */
#define END_WINDOW 1024U
static void IRAM_ATTR clear_end(uint32_t *b, unsigned start)
{
    unsigned at = (start + CAPTURE_UNIT - 2U * EDGE_GUARD) & RING_MASK;
    for (unsigned k = 0; k < END_WINDOW; k += 4, at = (at + 4U) & RING_MASK)
        b[at] = b[at + 1] = b[at + 2] = b[at + 3] = SENTINEL;   /* at stays a multiple of 4 */
    __asm__ volatile ("memw" ::: "memory");
}

static bool IRAM_ATTR handoff(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
                              void *arg)
{
    sched_t *h = arg;
    BaseType_t woken = pdFALSE;
    uint32_t start = esp_cpu_get_cycle_count(), now = start;
    for (;;) {
        unsigned at = REG_READ(DUMP_INDEX) & RING_MASK;
        h->pairs += (at - h->index) & RING_MASK;
        h->index = at;
        if (h->pairs >= h->target) break;
        now = esp_cpu_get_cycle_count();
        if (now - start > 4U * EARLY_US * 240U) { h->fault = "stalled"; goto wake; }
    }
    if (h->posted - h->done >= RECORDS) { h->fault = "consumer stuck"; goto wake; }
    unsigned next;
    bool taken = false;
    if (h->free_head != h->free_tail) {
        next = h->free[h->free_head & 3U];
        ++h->free_head;
    } else {
        uint32_t newest = h->posted - 1U;
        if ((int32_t)(newest - h->working) <= 0) { h->fault = "no bank"; goto wake; }
        h->dropped[newest % RECORDS] = 1;
        next = h->bank[newest % RECORDS];
        ++h->drops;
        taken = true;
        clear_end(bank(next), h->index & ~3U);
    }
    unsigned slot = h->posted % RECORDS;
    h->before[slot] = REG_READ(DUMP_INDEX) & RING_MASK;
    REG_WRITE(BANK_USAGE, h->owner | (1U << next));
    __asm__ volatile ("memw" ::: "memory");
    h->after[slot] = REG_READ(DUMP_INDEX) & RING_MASK;
    h->bank[slot] = h->current;
    h->reowned[slot] = h->current_reowned;
    h->current_reowned = taken;
    h->dropped[slot] = 0;
    h->at[slot] = h->pairs;
    uint32_t late = (uint32_t)(h->pairs - h->target) + ((h->after[slot] - h->index) & RING_MASK);
    if (late > h->max_late) h->max_late = late;
    if (late >= CAPTURE_LATE) { h->fault = "late"; goto wake; }
    ++h->posted;
    h->current = next;
    h->target += CAPTURE_UNIT;
    uint32_t spin = esp_cpu_get_cycle_count() - start;
    if (spin > h->max_spin) h->max_spin = spin;
    /* Re-anchor on the handoff just made: the dump clock need not match the timer's. */
    gptimer_alarm_config_t alarm = {
        .alarm_count = event->count_value + UNIT_TICKS - EARLY_TICKS
                       + spin / 24U,   /* cycles at 240 MHz to 0.1 us ticks */
    };
    gptimer_set_alarm_action(timer, &alarm);
wake:
    vTaskNotifyGiveFromISR(h->task, &woken);
    return woken == pdTRUE;
}

static const char *IRAM_ATTR stream(job_t *job)
{
    const capture_config_t *c = job->config;
    capture_stats_t *s = job->stats;
    sched_t h = {.owner = REG_READ(BANK_USAGE) & ~15U, .target = CAPTURE_UNIT,
                 .task = xTaskGetCurrentTaskHandle(), .free = {1, 2}, .free_tail = 2};
    gptimer_config_t tc = {.clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP,
                           .resolution_hz = TIMER_HZ, .intr_priority = 3};
    gptimer_event_callbacks_t callbacks = {.on_alarm = handoff};
    gptimer_alarm_config_t first = {.alarm_count = UNIT_TICKS - EARLY_TICKS};
    if (gptimer_new_timer(&tc, &h.timer) != ESP_OK) return "timer";
    const char *fault = NULL;
    if (gptimer_register_event_callbacks(h.timer, &callbacks, &h) != ESP_OK ||
        gptimer_enable(h.timer) != ESP_OK || gptimer_set_alarm_action(h.timer, &first) != ESP_OK) {
        fault = "timer";
        goto done;
    }
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(BANK_USAGE, h.owner);
    __asm__ volatile ("memw" ::: "memory");
    for (unsigned n = 0; n < 3; ++n) clear(bank(n));
    REG_WRITE(DUMP_LANES, 0x000c2040U);
    REG_WRITE(BANK_USAGE, h.owner | 1U);
    __asm__ volatile ("memw" ::: "memory");
    uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    REG_WRITE(DUMP_CTRL, DUMP_RUN | DUMP_CIRCULAR | DUMP_16M | CAPTURE_RING);
    gptimer_start(h.timer);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);

    const uint64_t total = (uint64_t)c->seconds * CAPTURE_RATE;
    unsigned before = 0, after = 0, expected = 0;
    uint64_t end_pairs = 0;     /* engine count at the end of the last unit processed */
    bool gap = false;
    while (s->samples < total && !h.fault) {
        if (h.done == h.posted) {
            if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))) { fault = "no handoff"; break; }
            continue;
        }
        uint32_t lag = h.posted - h.done;
        if (lag > s->max_lag) s->max_lag = lag;
        unsigned slot = h.done % RECORDS;
        uint32_t irq2 = portSET_INTERRUPT_MASK_FROM_ISR();
        h.working = h.done;
        bool dropped = h.dropped[slot];
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq2);
        if (dropped) {
            before = h.before[slot];
            after = h.after[slot];
            gap = true;
            ++h.done;
            continue;
        }
        uint32_t start = esp_cpu_get_cycle_count();
        const uint32_t *b = bank(h.bank[slot]);
        unsigned first = gap ? after : expected, last;
        /* A taken-back bank has no sentinels before its unit: it starts where the last unit
         * ended, or, after a loss, at the handoff bracket (a sample or two of slack). */
        if ((!h.reowned[slot] && !edge(b, before, after, true, &first)) ||
            !edge(b, h.before[slot], h.after[slot], false, &last)) { fault = "edge"; break; }
        size_t count = (last - first) & RING_MASK;
        if (first != expected && !gap) { fault = "gap"; break; }
        if (gap) {
            /* Lost units: the engine count gives the length to within a bracket and the
             * ring index makes it exact. */
            uint64_t approx = h.at[(h.done - 1U) % RECORDS] - end_pairs;
            uint64_t lost = approx + ((((first - expected - approx) & RING_MASK) + CAPTURE_RING / 2U)
                                      & RING_MASK) - CAPTURE_RING / 2U;
            s->samples += lost;
            s->dropped += lost;
            gap = false;
        }
        if (count + CAPTURE_LATE < CAPTURE_UNIT || count > CAPTURE_UNIT + CAPTURE_LATE) {
            fault = "span";
            break;
        }
        size_t head = CAPTURE_RING - first < count ? CAPTURE_RING - first : count;
        if (job->fn(b + first, head, s->samples, job->ctx) != ESP_OK ||
            (count > head && job->fn(b, count - head, s->samples + head, job->ctx) != ESP_OK)) {
            fault = "consumer";
            break;
        }
        uint32_t cleared = esp_cpu_get_cycle_count();
        clear((uint32_t *)b);
        uint32_t now = esp_cpu_get_cycle_count();
        if (now - cleared > s->max_clear) s->max_clear = now - cleared;
        if (now - start > s->max_service) s->max_service = now - start;
        s->samples += count;
        ++s->units;
        expected = last;
        end_pairs = h.at[slot];
        before = h.before[slot];
        after = h.after[slot];
        h.free[h.free_tail & 3U] = h.bank[slot];
        __asm__ volatile ("memw" ::: "memory");
        ++h.free_tail;
        ++h.done;
    }
    if (!fault) fault = h.fault;
    s->drops = h.drops;

done:
    REG_WRITE(DUMP_CTRL, 0);
    gptimer_stop(h.timer);
    gptimer_disable(h.timer);
    gptimer_del_timer(h.timer);
    s->max_late = h.max_late;
    s->max_spin = h.max_spin;
    return fault;
}

static uint32_t IRAM_ATTR read_words(const uint32_t *p, size_t words)
{
    uint32_t sum = 0;
    for (size_t k = 0; k < words; k += 4) sum += p[k] + p[k + 1] + p[k + 2] + p[k + 3];
    return sum;
}

static void IRAM_ATTR read_vectors(const uint32_t *p, size_t words)
{
    for (size_t k = 0; k < words; k += 16)
        __asm__ volatile ("ee.vld.128.ip q0, %0, 16\n ee.vld.128.ip q1, %0, 16\n"
                          "ee.vld.128.ip q2, %0, 16\n ee.vld.128.ip q3, %0, 16" : "+r"(p));
}

static bool copied(async_memcpy_handle_t mcp, async_memcpy_event_t *event, void *arg)
{
    (void)mcp; (void)event;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(arg, &woken);
    return woken == pdTRUE;
}

/* Cycles to read 4,096 words (16 KiB) from a free bank while the engine writes
 * another, against the same reads from ordinary internal RAM, then the engine
 * stopped. Slots: words, vectors, memcpy, gdma; bank then dram; running then stopped. */
static const char *bench(job_t *job)
{
    enum { WORDS = 4096 };
    uint32_t *r = job->bench, *dram = heap_caps_aligned_alloc(16, 4 * WORDS,
                                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    async_memcpy_handle_t mcp = NULL;
    async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    const char *fault = !dram || !sem ? "memory" :
                        esp_async_memcpy_install(&mc, &mcp) != ESP_OK ? "gdma" : NULL;
    const uint32_t owner = REG_READ(BANK_USAGE) & ~15U;
    for (unsigned run = 0; !fault && run < 2; ++run) {
        REG_WRITE(DUMP_CTRL, 0);
        REG_WRITE(BANK_USAGE, owner);
        __asm__ volatile ("memw" ::: "memory");
        clear(bank(1));
        if (run == 0) {
            REG_WRITE(DUMP_LANES, 0x000c2040U);
            REG_WRITE(BANK_USAGE, owner | 1U);
            REG_WRITE(DUMP_CTRL, DUMP_RUN | DUMP_CIRCULAR | DUMP_16M | CAPTURE_RING);
            vTaskDelay(2);
        }
        for (unsigned src = 0; src < 2; ++src, r += 4) {
            const uint32_t *from = src ? dram : bank(1) + 1024;
            uint32_t t = esp_cpu_get_cycle_count();
            volatile uint32_t sink = read_words(from, WORDS);
            (void)sink;
            r[0] = esp_cpu_get_cycle_count() - t;
            t = esp_cpu_get_cycle_count();
            read_vectors(from, WORDS);
            r[1] = esp_cpu_get_cycle_count() - t;
            if (!src) {
                t = esp_cpu_get_cycle_count();
                memcpy(dram, from, 4 * WORDS);
                r[2] = esp_cpu_get_cycle_count() - t;
                t = esp_cpu_get_cycle_count();
                if (esp_async_memcpy(mcp, dram, (void *)from, 4 * WORDS, copied, sem) != ESP_OK ||
                    xSemaphoreTake(sem, pdMS_TO_TICKS(10)) != pdTRUE) fault = "gdma copy";
                r[3] = esp_cpu_get_cycle_count() - t;
            }
        }
    }
    REG_WRITE(DUMP_CTRL, 0);
    if (mcp) esp_async_memcpy_uninstall(mcp);
    if (sem) vSemaphoreDelete(sem);
    heap_caps_free(dram);
    return fault;
}

static void worker(void *arg)
{
    job_t *job = arg;
    const capture_config_t *c = job->config;
    capture_stats_t *s = job->stats;
    const bool divided = c->mhz < 2200U;
    const uint32_t pll_khz = c->mhz * (divided ? 1200U : 1000U);
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    force_txon_mode(0, 0, 0);
    REG_CLR_BIT(FE_CTRL, 1U << 1);
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    const uint8_t lo_saved = regi2c_ctrl_read_reg(0x65, 0, 0);
    const uint32_t bb_saved = REG_READ(SYSCON_WIFI_BB_CFG_REG);
    select_lo(false);
    set_chanfreq(2412, 0);
    phy_set_freq(pll_khz / 1000U, pll_khz % 1000U);
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    s->gain_max = (REG_READ(AGC_FORCE) >> 8) & 0x7f;
    if (c->gain > s->gain_max) {
        s->error = ESP_ERR_INVALID_ARG;
    } else {
        force_rx_gain(1, c->gain, 0);
        select_lo(divided);
        /* Bit 1 is set while Wi-Fi runs; without it the dump holds a constant. */
        REG_SET_BIT(SYSCON_WIFI_BB_CFG_REG, 1U << 1);
        vTaskDelay(pdMS_TO_TICKS(3));
        s->free_ready = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s->largest_ready = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s->fault = job->bench ? bench(job) : stream(job);
        if (s->fault) s->error = ESP_FAIL;
        s->min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        s->stack_free = uxTaskGetStackHighWaterMark(NULL);
    }
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    REG_CLR_BIT(FE_CTRL, 1U << 1);
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(0);
    rom_set_rxclk_en(0);
    select_lo(lo_saved & 0x10U);
    REG_WRITE(SYSCON_WIFI_BB_CFG_REG, bb_saved);
    xSemaphoreGive(job->done);
    vTaskSuspend(NULL);     /* the launcher deletes it, so its static buffers are free again */
}

static esp_err_t launch(job_t *job);

esp_err_t capture_run(const capture_config_t *config, capture_fn fn, void *ctx,
                      capture_stats_t *stats)
{
    memset(stats, 0, sizeof *stats);
    if (config->mhz < 2000U || config->mhz > 2450U || !config->seconds || config->seconds > 600U)
        return stats->error = ESP_ERR_INVALID_ARG;
    job_t job = {config, fn, ctx, stats, NULL, NULL};
    return launch(&job);
}

esp_err_t capture_bench(const capture_config_t *config, uint32_t results[16],
                        capture_stats_t *stats)
{
    memset(stats, 0, sizeof *stats);
    memset(results, 0, 16 * sizeof *results);
    job_t job = {config, NULL, NULL, stats, results, NULL};
    return launch(&job);
}

static esp_err_t launch(job_t *job)
{
    capture_stats_t *s = job->stats;
    bool initialized = false, held = false;
    /* Wi-Fi starts only to power and calibrate the PHY. The run then holds the PHY with
     * the Bluetooth modem bit, as firmware/components/radio_s3/physolo.c does for the
     * transmitter, keeps the Wi-Fi clock domain referenced and deinitializes Wi-Fi: no MAC
     * interrupts or periodic work, and about 44 KB of internal RAM return. The worker
     * sets the baseband bit that Wi-Fi's stop clears. */
    s->free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    if ((s->error = esp_wifi_init(&cfg)) != ESP_OK) goto done;
    initialized = true;
    if ((s->error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (s->error = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK ||
        (s->error = esp_wifi_start()) != ESP_OK) goto done;
    if (phy_get_modem_flag() != PHY_MODEM_WIFI) { s->error = ESP_ERR_INVALID_STATE; goto done; }
    esp_wifi_bt_power_domain_on();
    esp_phy_modem_init();
    esp_phy_enable(PHY_MODEM_BT);
    wifi_module_enable();
    held = true;
    if ((s->error = esp_wifi_stop()) != ESP_OK || (s->error = esp_wifi_deinit()) != ESP_OK) goto done;
    initialized = false;
    s->free_wifi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (job->config->ready && (s->error = job->config->ready(job->ctx)) != ESP_OK) goto done;
    if (!(job->done = xSemaphoreCreateBinary())) { s->error = ESP_ERR_NO_MEM; goto done; }
    /* A static stack stays in internal SRAM; from the heap it could land in slow RTC FAST
     * memory, where the demodulator's stack traffic once ran 3.4 times slower. */
    static StackType_t stack[CAPTURE_STACK / sizeof(StackType_t)];
    static StaticTask_t task;
    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(worker, "capture", CAPTURE_STACK, job,
                                                        configMAX_PRIORITIES - 1, stack, &task, 1);
    if (!handle) {
        s->error = ESP_ERR_NO_MEM;
        goto done;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);
    while (eTaskGetState(handle) != eSuspended) vTaskDelay(1);
    vTaskDelete(handle);

done:
    if (job->done) vSemaphoreDelete(job->done);
    if (initialized) {
        esp_err_t e = esp_wifi_stop();
        if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_STARTED) s->cleanup = e;
        if ((e = esp_wifi_deinit()) != ESP_OK && s->cleanup == ESP_OK) s->cleanup = e;
    }
    if (held) {
        esp_phy_disable(PHY_MODEM_BT);
        esp_phy_modem_deinit();
        wifi_module_disable();
        esp_wifi_bt_power_domain_off();
    }
    return s->error;
}
