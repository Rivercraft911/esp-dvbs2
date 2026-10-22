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
#include "driver/gptimer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"

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
 * re-owns a bank two handoffs after releasing it; the interrupt faults instead of
 * handing back a bank the task has not finished. */
#define TIMER_HZ 10000000U
#define EARLY_US 15U            /* alarm this far before a handoff, then spin */
#define UNIT_TICKS ((uint64_t)CAPTURE_UNIT * TIMER_HZ / CAPTURE_RATE)
#define EARLY_TICKS ((uint64_t)EARLY_US * (TIMER_HZ / 1000000U))
typedef struct {
    gptimer_handle_t timer;
    uint32_t owner, current, max_late, max_spin;
    uint16_t index, before[4], after[4];
    uint8_t bank[4];
    uint64_t pairs, target;
    volatile uint32_t posted, done;
    const char *volatile fault;
    TaskHandle_t task;
} sched_t;

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
    if (h->posted - h->done >= 2) { h->fault = "consumer late"; goto wake; }
    unsigned next = (h->current + 1U) % 3U, slot = h->posted & 3U;
    h->before[slot] = REG_READ(DUMP_INDEX) & RING_MASK;
    REG_WRITE(BANK_USAGE, h->owner | (1U << next));
    __asm__ volatile ("memw" ::: "memory");
    h->after[slot] = REG_READ(DUMP_INDEX) & RING_MASK;
    h->bank[slot] = h->current;
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
                 .task = xTaskGetCurrentTaskHandle()};
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
    while (s->samples < total && !h.fault) {
        if (h.done == h.posted) {
            if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))) { fault = "no handoff"; break; }
            continue;
        }
        uint32_t lag = h.posted - h.done;
        if (lag > s->max_lag) s->max_lag = lag;
        unsigned slot = h.done & 3U;
        uint32_t start = esp_cpu_get_cycle_count();
        const uint32_t *b = bank(h.bank[slot]);
        unsigned first, last;
        if (!edge(b, before, after, true, &first) ||
            !edge(b, h.before[slot], h.after[slot], false, &last)) { fault = "edge"; break; }
        size_t count = (last - first) & RING_MASK;
        if (first != expected) { fault = "gap"; break; }
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
        before = h.before[slot];
        after = h.after[slot];
        ++h.done;
    }
    if (!fault) fault = h.fault;

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
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
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
    bool initialized = false, started = false;
    /* Wi-Fi only powers the PHY. Starting it here, on the console core, keeps its
     * interrupt off core 1. Deinitializing it (with the PHY held) froze the dump
     * data at a constant, so it stays up for the whole run. */
    s->free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    if ((s->error = esp_wifi_init(&cfg)) != ESP_OK) goto done;
    initialized = true;
    if ((s->error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (s->error = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK ||
        (s->error = esp_wifi_start()) != ESP_OK) goto done;
    started = true;
    if ((s->error = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) goto done;
    if (job->config->promiscuous &&
        ((s->error = esp_wifi_set_promiscuous(true)) != ESP_OK ||
         (s->error = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)) != ESP_OK)) goto done;
    s->free_wifi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (job->config->ready && (s->error = job->config->ready(job->ctx)) != ESP_OK) goto done;
    if (!(job->done = xSemaphoreCreateBinary())) { s->error = ESP_ERR_NO_MEM; goto done; }
    if (xTaskCreatePinnedToCore(worker, "capture", CAPTURE_STACK, job, configMAX_PRIORITIES - 1,
                                NULL, 1) != pdPASS) {
        s->error = ESP_ERR_NO_MEM;
        goto done;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);

done:
    if (job->done) vSemaphoreDelete(job->done);
    if (started) esp_wifi_set_promiscuous(false);
    if (initialized) {
        esp_err_t e = esp_wifi_stop();
        if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_STARTED) s->cleanup = e;
        if ((e = esp_wifi_deinit()) != ESP_OK && s->cleanup == ESP_OK) s->cleanup = e;
    }
    return s->error;
}
