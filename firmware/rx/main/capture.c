/* Independent finite RX experiment. Register facts are documented at the pinned
 * URL in samples.c, sections "Dump and DAC engine", "SRAM bank ownership" and
 * "Vendor library functions as used". The capture engine, bank handoff bounds,
 * 16 MS/s rate and PSRAM deadline are NOT qualified until measured on a board.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capture.h"
#include "spill.h"
#include "esp_attr.h"
#include "esp_async_memcpy.h"
#include "esp_cpu.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/regi2c_bbpll.h"

#define DUMP_CTRL 0x60033d5cU
#define DUMP_INDEX 0x60033d60U
#define DAC_CTRL 0x60033d64U
#define DUMP_LANES 0x60033d90U
#define BANK_USAGE 0x600c101cU
#define AGC_FORCE 0x6001c02cU
#define FE_CTRL 0x60006000U
#define UNIT_PAIRS 12288U
#define LATE_PAIRS 2000U
#define PAIR_CYCLES 15U
#define CONTROL_PAIRS 512U

SOC_RESERVE_MEMORY_REGION(0x3fcb0000, 0x3fce0000, radio_rx_banks);

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
    uint8_t *record;
    uint32_t *snapshot;
    rx_report *report;
    uint16_t mhz;
    uint8_t gain;
    bool probe, pattern;
    unsigned dma_burst;
    rx_memory_report *memory;
    rx_adc_report *adc;
    rx_spill *spill;
    SemaphoreHandle_t done;
} run;

static uint32_t *IRAM_ATTR bank(unsigned n)
{
    return (uint32_t *)(0x3fcb0000U + n * 65536U);
}

/* CKGEN 0x65:0[4], pinned register notes in samples.c: "LO 5/6 divider".
 * Calibrate with direct conversion; select the RX divider after gain setup. */
static bool lo_select(bool divided, uint8_t *actual)
{
    uint8_t before = regi2c_ctrl_read_reg(0x65, 0, 0);
    uint8_t expected = rx_lo_value(before, divided);
    if (expected != before) regi2c_ctrl_write_reg_mask(0x65, 0, 0, 4, 4, divided);
    *actual = regi2c_ctrl_read_reg(0x65, 0, 0);
    return *actual == expected;
}

static void IRAM_ATTR clear_bank(unsigned n)
{
    uint32_t *p = bank(n);
    for (unsigned i = 0; i < RX_BANK_PAIRS; i += 8) {
        p[i] = p[i + 1] = p[i + 2] = p[i + 3] = RX_SENTINEL;
        p[i + 4] = p[i + 5] = p[i + 6] = p[i + 7] = RX_SENTINEL;
    }
    __asm__ volatile ("memw" ::: "memory");
}

static esp_err_t IRAM_ATTR copy_unit(run *r, unsigned current, uint16_t first,
                                    size_t count, unsigned pending)
{
    rx_report *s = r->report;
    uint32_t before = esp_cpu_get_cycle_count();
    esp_err_t e = rx_spill_begin(r->spill, bank(current), first, count,
                                (uint32_t *)r->record + s->retained_pairs);
    uint32_t prepared = esp_cpu_get_cycle_count();
    if (e != ESP_OK) { ++s->dma_errors; return e; }
    uint32_t clear_cycles = 0;
    /* The previous DMA source is complete and CPU-owned. Clear it while this
     * DMA reads a different bank; it is ready before the next ownership switch. */
    if (pending < 3) {
        clear_bank(pending);
        clear_cycles = esp_cpu_get_cycle_count() - prepared;
    }
    e = rx_spill_finish(r->spill);
    uint32_t elapsed = esp_cpu_get_cycle_count() - before;
    s->dma_rx_flags |= r->spill->rx_flags;
    s->dma_tx_flags |= r->spill->tx_flags;
    if (prepared - before > s->max_dma_setup_cycles) s->max_dma_setup_cycles = prepared - before;
    if (clear_cycles > s->max_clear_cycles) s->max_clear_cycles = clear_cycles;
    if (elapsed > s->max_copy_cycles) s->max_copy_cycles = elapsed;
    if (elapsed > s->max_raw_copy_cycles) {
        s->max_raw_copy_cycles = elapsed;
        s->max_copy_pairs = count;
    }
    if (e != ESP_OK) ++s->dma_errors;
    else ++s->dma_copies;
    return e;
}

typedef struct {
    uint32_t pairs, cycle, changed;
    uint16_t index;
} position;

static bool IRAM_ATTR advance(position *p, rx_report *s)
{
    uint32_t now = esp_cpu_get_cycle_count();
    uint32_t elapsed = now - p->cycle;
    uint16_t index = REG_READ(DUMP_INDEX) & RX_BANK_MASK;
    if (elapsed > s->max_poll_cycles) s->max_poll_cycles = elapsed;
    /* A full unobserved revolution would make the modulo index ambiguous. */
    if (elapsed >= (RX_BANK_PAIRS - RX_EDGE_GUARD) * PAIR_CYCLES) {
        ++s->overruns;
        return false;
    }
    unsigned delta = (index - p->index) & RX_BANK_MASK;
    p->pairs += delta;
    p->index = index;
    p->cycle = now;
    if (delta) p->changed = now;
    if (now - p->changed >= RX_BANK_PAIRS * PAIR_CYCLES) {
        ++s->overruns; /* The engine stopped or its index is no longer advancing. */
        return false;
    }
    return true;
}

static esp_err_t IRAM_ATTR __attribute__((noinline)) collect(run *r)
{
    rx_report *s = r->report;
    const uint32_t ownership = REG_READ(BANK_USAGE) & ~15U;
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    REG_WRITE(BANK_USAGE, ownership);
    __asm__ volatile ("memw" ::: "memory");
    for (unsigned n = 0; n < 3; ++n) clear_bank(n);
    REG_WRITE(DUMP_LANES, 0x000c2040U);
    REG_WRITE(BANK_USAGE, ownership | 1U);

    /* Capture is at most ~126 ms. Keep the Wi-Fi MAC ISR and scheduler out of the
     * bank handoffs; core 0 remains available for the kernel and USB. The default
     * configured interrupt-watchdog limit is unchanged. No prints/USB/allocations here. */
    uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t epoch = esp_cpu_get_cycle_count();
    if (r->probe) s->probe.before_start = REG_READ(DUMP_INDEX);
    REG_WRITE(DUMP_CTRL, (1U << 31) | (1U << 17) | (1U << 16) | RX_BANK_PAIRS);
    if (r->probe) {
        s->probe.after_start = REG_READ(DUMP_INDEX);
        s->probe.start_cycles = esp_cpu_get_cycle_count() - epoch;
        s->probe.control = REG_READ(DUMP_CTRL);
        s->probe.lanes = REG_READ(DUMP_LANES);
        s->probe.bank_start = REG_READ(BANK_USAGE);
    }
    position p = {.cycle = epoch, .changed = epoch};
    rx_edge begin = {0, 0};
    uint32_t target = UNIT_PAIRS;
#if RX_GAIN_REFRESH_CYCLES
    uint32_t refresh = epoch;
#endif
    uint16_t expected = 0;
    unsigned current = 0, pending = 3;
    esp_err_t error = ESP_FAIL;
    while (s->retained_pairs < RX_RECORD_PAIRS) {
        if (!advance(&p, s)) break;
        if (p.pairs < target) continue;
        uint32_t late = p.pairs - target;
        if (late > s->max_switch_late_pairs) s->max_switch_late_pairs = late;
        if (late >= LATE_PAIRS) { ++s->late; break; }
        unsigned next = (current + 1U) % 3U;
        /* Unrelated accounting can exceed the edge limit. Read immediately
         * around the ownership write instead; keep the same64-pair bound. */
        rx_edge end = {.before = REG_READ(DUMP_INDEX) & RX_BANK_MASK};
        REG_WRITE(BANK_USAGE, ownership | (1U << next));
        __asm__ volatile ("memw" ::: "memory");
        end.after = REG_READ(DUMP_INDEX) & RX_BANK_MASK;
        uint32_t switch_cycles = esp_cpu_get_cycle_count() - epoch;
        late += (end.after - p.index) & RX_BANK_MASK;
        if (late > s->max_switch_late_pairs) s->max_switch_late_pairs = late;
        if (late >= LATE_PAIRS) { ++s->late; break; }
        if (!advance(&p, s)) break;

        if (r->probe) {
            s->probe.before_switch = end.before;
            s->probe.after_switch = end.after;
            s->probe.switch_cycles = switch_cycles;
            s->probe.bank_switch = REG_READ(BANK_USAGE);
            uint16_t first = 0, last = 0;
            if (rx_find_edge(bank(current), begin, true, &first)) s->probe.edges |= 1U;
            if (rx_find_edge(bank(current), end, false, &last)) s->probe.edges |= 2U;
            s->probe.first = first;
            s->probe.end = last;
            s->probe.before_stop = REG_READ(DUMP_INDEX);
            REG_WRITE(DUMP_CTRL, 0);
            s->engine_cycles = esp_cpu_get_cycle_count() - epoch;
            s->probe.after_stop = REG_READ(DUMP_INDEX);
            REG_WRITE(BANK_USAGE, ownership);
            __asm__ volatile ("memw" ::: "memory");
            /* Geometry-only: preserve every word, including untouched sentinel,
             * in the three allowed banks. No edge inference or gap repair. */
            for (unsigned n = 0; n < 3; ++n)
                memcpy(r->record + n * 65536U, bank(n), 65536U);
            s->probe.bytes = 3U * 65536U;
            error = ESP_OK;
            break;
        }

        uint16_t first;
        size_t count;
        const uint32_t *src = bank(current);
        if (!rx_span(src, begin, end, &first, &count)) { ++s->boundary_errors; break; }
        if (count < UNIT_PAIRS - LATE_PAIRS || count > UNIT_PAIRS + LATE_PAIRS) {
            ++s->boundary_errors;
            break;
        }
        if (first != expected) { ++s->gaps; break; }
        expected = (first + count) & RX_BANK_MASK;
        s->acquired_pairs += count;
        ++s->units;
        size_t keep = RX_RECORD_PAIRS - s->retained_pairs;
        if (keep > count) keep = count;
        if (keep == RX_RECORD_PAIRS - s->retained_pairs) {
            REG_WRITE(DUMP_CTRL, 0);
            s->engine_cycles = esp_cpu_get_cycle_count() - epoch;
        }

        if (copy_unit(r, current, first, keep, pending) != ESP_OK) break;
        s->retained_pairs += keep;
        pending = current;
        if (s->retained_pairs == RX_RECORD_PAIRS) { error = ESP_OK; break; }

#if RX_GAIN_REFRESH_CYCLES
        /* Vendor RX gain can drift during an extended capture. Refresh at bank
         * boundaries and retain the resulting worst-case poll/late measurements. */
        if (esp_cpu_get_cycle_count() - refresh >= RX_GAIN_REFRESH_CYCLES) {
            force_rx_gain(1, r->gain, 0);
            refresh = esp_cpu_get_cycle_count();
            ++s->gain_refreshes;
        }
#endif
        begin = end;
        current = next;
        target += UNIT_PAIRS;
    }
    REG_WRITE(DUMP_CTRL, 0);
    if (!s->engine_cycles) s->engine_cycles = esp_cpu_get_cycle_count() - epoch;
    REG_WRITE(BANK_USAGE, ownership);
    if (error == ESP_OK && pending < 3) clear_bank(pending);
    s->elapsed_cycles = esp_cpu_get_cycle_count() - epoch;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return error;
}

static void IRAM_ATTR __attribute__((noinline)) pattern_run(run *r)
{
    rx_report *s = r->report;
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
    __asm__ volatile ("memw" ::: "memory");
    uint32_t epoch = esp_cpu_get_cycle_count();
    for (unsigned n = 0; n < 3; ++n) clear_bank(n);
    unsigned pending = 3;
    for (unsigned unit = 0; s->retained_pairs < RX_RECORD_PAIRS; ++unit) {
        unsigned current = unit % 3U, first = s->retained_pairs & RX_BANK_MASK;
        size_t count = RX_RECORD_PAIRS - s->retained_pairs;
        /* Odd units exercise every input/output cache-line alignment. */
        if (count > UNIT_PAIRS + 1U) count = UNIT_PAIRS + 1U;
        for (size_t n = 0; n < count; ++n) {
            uint32_t at = s->retained_pairs + n;
            bank(current)[(first + n) & RX_BANK_MASK] = ((at * 13U) & 1023U) << 10 |
                                                       ((at * 29U + 7U) & 1023U);
        }
        /* Same core, PSRAM write order, wrap phases and full sentinel clear as
         * capture. Mask only the measured copy/clear, leaving IRQs between units. */
        uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
        esp_err_t error = copy_unit(r, current, first, count, pending);
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
        if (error != ESP_OK) { s->error = error; return; }
        pending = current;
        s->retained_pairs += count;
    }
    if (pending < 3) clear_bank(pending);
    s->elapsed_cycles = esp_cpu_get_cycle_count() - epoch;
    s->error = ESP_OK;
}

static uint32_t memory_word(unsigned n, unsigned source)
{
    return 0x9e3779b9U * (n + 1U) ^ (0x6d2b79f5U * (source + 1U));
}

static void IRAM_ATTR __attribute__((noinline)) memory_run(run *r)
{
    static const struct { const char *name; int bank; unsigned destination; } cases[] = {
        {"internal_internal", -1, 0},
        {"bank0_internal", 0, 0}, {"bank1_internal", 1, 0}, {"bank2_internal", 2, 0},
        {"internal_psram_hot", -1, 1}, {"internal_psram_stream", -1, 2},
        {"bank0_psram_stream", 0, 2},
    };
    enum { chunk = 1024, units = 128, chunks = UNIT_PAIRS / chunk };
    uint8_t *allocation = heap_caps_malloc(2U * chunk * 4U + 32U,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!allocation) { r->report->error = ESP_ERR_NO_MEM; return; }
    memset(allocation, 0x6d, 2U * chunk * 4U + 32U);
    uint32_t *expected = (uint32_t *)(allocation + 16U), *scratch = expected + chunk;
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
    __asm__ volatile ("memw" ::: "memory");
    for (unsigned b = 0; b < 3; ++b)
        for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) bank(b)[n] = memory_word(n, b);

    for (unsigned c = 0; c < RX_MEMORY_CASES; ++c) {
        rx_memory_result *s = &r->memory->cases[c];
        s->name = cases[c].name;
        s->min_cycles = UINT32_MAX;
        if (cases[c].destination == 2)
            memset(r->record, 0x6d, units * UNIT_PAIRS * 4U);
        for (unsigned unit = 0; unit < units; ++unit) {
            uint32_t unit_cycles = 0;
            for (unsigned part = 0; part < chunks; ++part) {
                unsigned first = cases[c].bank < 0 ? 0 : part * chunk;
                unsigned source = cases[c].bank < 0 ? 3U : (unsigned)cases[c].bank;
                for (unsigned n = 0; n < chunk; ++n)
                    expected[n] = memory_word(first + n, source);
                const uint32_t *src = cases[c].bank < 0 ? expected : bank(cases[c].bank);
                uint32_t *out = cases[c].destination ? (uint32_t *)r->record : scratch;
                if (cases[c].destination == 2) out += unit * UNIT_PAIRS + part * chunk;
                else memset(out, 0x6d, chunk * 4U);

                uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
                uint32_t before = esp_cpu_get_cycle_count();
                rx_copy(src, first, chunk, out);
                __asm__ volatile ("memw" ::: "memory");
                uint32_t elapsed = esp_cpu_get_cycle_count() - before;
                portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
                unit_cycles += elapsed;
                if (elapsed > s->max_chunk_cycles) s->max_chunk_cycles = elapsed;
                uint32_t got = esp_rom_crc32_le(0, (uint8_t *)out, chunk * 4U);
                uint32_t wanted = esp_rom_crc32_le(0, (uint8_t *)expected, chunk * 4U);
                s->errors += got != wanted;
                s->crc32 = esp_rom_crc32_le(s->crc32, (uint8_t *)out, chunk * 4U);
                s->expected_crc32 = esp_rom_crc32_le(s->expected_crc32,
                                                    (uint8_t *)expected, chunk * 4U);
            }
            if (unit_cycles < s->min_cycles) s->min_cycles = unit_cycles;
            if (unit_cycles > s->max_cycles) s->max_cycles = unit_cycles;
            s->total_cycles += unit_cycles;
            ++s->units;
        }
        if (s->errors || s->crc32 != s->expected_crc32) r->report->error = ESP_FAIL;
    }
    r->memory->guards_ok = true;
    for (unsigned n = 0; n < 16; ++n)
        if (allocation[n] != 0x6d || allocation[16U + 2U * chunk * 4U + n] != 0x6d)
            r->memory->guards_ok = false;
    if (!r->memory->guards_ok) r->report->error = ESP_FAIL;
    free(allocation);
}

typedef struct {
    SemaphoreHandle_t done;
    volatile uint32_t finished;
    volatile uint32_t calls;
} dma_completion;

static bool IRAM_ATTR dma_done(async_memcpy_handle_t handle, async_memcpy_event_t *event,
                               void *arg)
{
    dma_completion *completion = arg;
    completion->finished = esp_cpu_get_cycle_count();
    ++completion->calls;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(completion->done, &woken);
    return woken == pdTRUE;
}

static void dma_run(run *r)
{
    static const char *names[] = {"internal_psram_dma4k", "bank0_psram_dma",
                                  "bank1_psram_dma", "bank2_psram_dma"};
    uint8_t *allocation = heap_caps_malloc(4096U + 128U,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!allocation) { r->report->error = ESP_ERR_NO_MEM; return; }
    memset(allocation, 0x6d, 4096U + 128U);
    uint32_t *internal = (uint32_t *)(((uintptr_t)allocation + 79U) & ~(uintptr_t)63U);
    for (unsigned n = 0; n < 1024; ++n) internal[n] = memory_word(n, 3);
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
    __asm__ volatile ("memw" ::: "memory");
    for (unsigned b = 0; b < 3; ++b)
        for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) bank(b)[n] = memory_word(n, b);
    async_memcpy_handle_t handle;
    async_memcpy_config_t config = ASYNC_MEMCPY_DEFAULT_CONFIG();
    config.backlog = 1;
    config.dma_burst_size = r->dma_burst;
    r->report->error = esp_async_memcpy_install_gdma_ahb(&config, &handle);
    if (r->report->error != ESP_OK) { free(allocation); return; }
    StaticSemaphore_t storage;
    dma_completion completion = {.done = xSemaphoreCreateBinaryStatic(&storage)};
    uint8_t *start = (uint8_t *)(((uintptr_t)r->record + 127U) & ~(uintptr_t)63U);
    r->memory->guards_ok = true;
    for (unsigned c = 0; c < 4 && r->report->error == ESP_OK; ++c) {
        rx_memory_result *s = &r->memory->cases[c];
        s->name = names[c];
        s->min_cycles = UINT32_MAX;
        size_t bytes = c ? UNIT_PAIRS * 4U : 4096U;
        uint32_t *src = c ? bank(c - 1U) : internal;
        uint32_t wanted = esp_rom_crc32_le(0, (uint8_t *)src, bytes);
        completion.calls = 0;
        memset(start - 64U, 0x6d, 128U * bytes + 128U);
        for (unsigned unit = 0; unit < 128; ++unit) {
            uint8_t *out = start + unit * bytes;
            uint32_t before = esp_cpu_get_cycle_count();
            r->report->error = esp_async_memcpy(handle, out, src, bytes, dma_done, &completion);
            uint32_t submitted = esp_cpu_get_cycle_count() - before;
            if (r->report->error != ESP_OK) break;
            /* The driver invalidates before DMA and completes its PSRAM barrier
             * before callback. Do not touch out or mask its completion IRQ. */
            if (xSemaphoreTake(completion.done, pdMS_TO_TICKS(2000)) != pdTRUE) {
                printf("@{\"cmd\":\"dmabench\",\"error\":\"DMA timeout; rebooting spare\"}\n");
                fflush(stdout);
                esp_restart();
            }
            uint32_t elapsed = esp_cpu_get_cycle_count() - before;
            uint32_t callback = completion.finished - before;
            if (elapsed < s->min_cycles) s->min_cycles = elapsed;
            if (elapsed > s->max_cycles) s->max_cycles = elapsed;
            if (submitted > s->max_submit_cycles) s->max_submit_cycles = submitted;
            if (callback > s->max_callback_cycles) s->max_callback_cycles = callback;
            s->max_chunk_cycles = s->max_cycles;
            s->total_cycles += elapsed;
            ++s->units;
            s->errors += esp_rom_crc32_le(0, out, bytes) != wanted;
            s->crc32 = esp_rom_crc32_le(s->crc32, out, bytes);
            s->expected_crc32 = esp_rom_crc32_le(s->expected_crc32, (uint8_t *)src, bytes);
        }
        for (unsigned n = 0; n < 64; ++n)
            if ((start - 64U)[n] != 0x6d || start[128U * bytes + n] != 0x6d)
                r->memory->guards_ok = false;
        s->callbacks = completion.calls;
        if (s->errors || s->crc32 != s->expected_crc32 || s->callbacks != s->units ||
            !r->memory->guards_ok)
            r->report->error = ESP_FAIL;
    }
    esp_err_t cleanup = esp_async_memcpy_uninstall(handle);
    if (cleanup != ESP_OK) {
        printf("@{\"cmd\":\"dmabench\",\"error\":\"DMA cleanup failed; rebooting spare\"}\n");
        fflush(stdout);
        esp_restart();
    }
    for (unsigned n = 0; n < 16; ++n)
        if (((uint8_t *)internal - 16U)[n] != 0x6d || ((uint8_t *)internal)[4096U + n] != 0x6d)
            r->memory->guards_ok = false;
    if (!r->memory->guards_ok) r->report->error = ESP_FAIL;
    free(allocation);
}

static void IRAM_ATTR __attribute__((noinline)) adc_run(run *r)
{
    rx_adc_report *s = r->adc;
    const uint32_t ownership = REG_READ(BANK_USAGE) & ~15U;
    s->saved_register = regi2c_ctrl_read_reg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_DIV_ADC);
    unsigned saved = (s->saved_register >> I2C_BBPLL_DIV_ADC_LSB) & 3U;
    for (unsigned step = 0; step < RX_ADC_STEPS; ++step) {
        rx_adc_step *p = &s->steps[step];
        p->code = step == 0 || step == RX_ADC_STEPS - 1 ? saved : step - 1U;
        REG_WRITE(DUMP_CTRL, 0);
        REG_WRITE(DAC_CTRL, 0);
        REG_WRITE(BANK_USAGE, ownership);
        __asm__ volatile ("memw" ::: "memory");
        regi2c_ctrl_write_reg_mask(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_DIV_ADC,
                                  I2C_BBPLL_DIV_ADC_MSB, I2C_BBPLL_DIV_ADC_LSB, p->code);
        esp_rom_delay_us(20);
        p->register_value = regi2c_ctrl_read_reg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_DIV_ADC);
        if (p->register_value != ((s->saved_register & ~12U) | (p->code << 2)))
            p->error = ESP_FAIL;
        for (unsigned b = 0; b < 3; ++b) clear_bank(b);
        REG_WRITE(DUMP_LANES, 0x000c2040U);
        REG_WRITE(BANK_USAGE, ownership | 1U);
        uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
        REG_WRITE(DUMP_CTRL, (1U << 31) | (1U << 17) | (1U << 16) | RX_BANK_PAIRS);
        uint16_t previous = REG_READ(DUMP_INDEX) & RX_BANK_MASK;
        p->first_index = previous;
        uint32_t first = esp_cpu_get_cycle_count(), last = first;
        while (p->pairs < RX_ADC_WORDS) {
            uint16_t index = REG_READ(DUMP_INDEX) & RX_BANK_MASK;
            uint32_t now = esp_cpu_get_cycle_count();
            uint32_t poll = now - last;
            if (poll > p->max_poll_cycles) p->max_poll_cycles = poll;
            p->pairs += (index - previous) & RX_BANK_MASK;
            previous = index;
            last = now;
            if (now - first >= 24000000U) { p->error = ESP_ERR_TIMEOUT; break; }
        }
        p->last_index = previous;
        p->cycles = last - first;
        REG_WRITE(DUMP_CTRL, 0);
        REG_WRITE(BANK_USAGE, ownership);
        __asm__ volatile ("memw" ::: "memory");
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
        /* Freeze first, then inspect all banks. A full or disjoint bank rejects
         * ambiguous wraps; raw snapshots retain untouched sentinel words. */
        for (unsigned n = 0; n < RX_BANK_PAIRS; ++n)
            if (bank(0)[n] != RX_SENTINEL) p->written_words = n + 1U;
        for (unsigned n = 0; n < p->written_words; ++n) p->holes += bank(0)[n] == RX_SENTINEL;
        for (unsigned b = 1; b < 3; ++b)
            for (unsigned n = 0; n < RX_BANK_PAIRS; ++n)
                p->other_bank_words += bank(b)[n] != RX_SENTINEL;
        if ((p->written_words < RX_ADC_WORDS || p->written_words == RX_BANK_PAIRS ||
             p->holes || p->other_bank_words) && p->error == ESP_OK) p->error = ESP_FAIL;
        uint8_t *out = r->record + step * RX_ADC_WORDS * 4U;
        memcpy(out, bank(0), RX_ADC_WORDS * 4U);
        p->crc32 = esp_rom_crc32_le(0, out, RX_ADC_WORDS * 4U);
        ++s->completed;
        if (p->error != ESP_OK) r->report->error = p->error;
    }
    regi2c_ctrl_write_reg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_DIV_ADC, s->saved_register);
    s->restored_register = regi2c_ctrl_read_reg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_DIV_ADC);
    if (s->restored_register != s->saved_register) r->report->error = ESP_FAIL;
    s->bytes = s->completed * RX_ADC_WORDS * 4U;
    s->crc32 = esp_rom_crc32_le(0, r->record, s->bytes);
}

static void worker(void *arg)
{
    run *r = arg;
    rx_report *s = r->report;
    bool initialized = false, started = false, rf_setup = false, lo_saved = false;
    rx_spill spill = {0};
    if (r->memory) {
        if (r->dma_burst) dma_run(r);
        else memory_run(r);
        goto done;
    }
    if (!r->adc && !r->probe) {
        r->spill = &spill;
        s->error = rx_spill_open(&spill);
        if (s->error != ESP_OK) goto done;
    }
    if (r->pattern) { pattern_run(r); goto done; }
    if (!rx_plan_lo(r->mhz, &s->tuning)) { s->error = ESP_ERR_INVALID_ARG; goto done; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    s->error = esp_wifi_init(&cfg);
    if (s->error != ESP_OK) goto done;
    initialized = true;
    if ((s->error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (s->error = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK ||
        (s->error = esp_wifi_start()) != ESP_OK) goto done;
    started = true;
    if ((s->error = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK ||
        (s->error = esp_wifi_set_promiscuous(true)) != ESP_OK ||
        (s->error = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)) != ESP_OK) goto done;

    rf_setup = true;
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    force_txon_mode(0, 0, 0);
    REG_CLR_BIT(FE_CTRL, 1U << 1);
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    s->lo_saved = regi2c_ctrl_read_reg(0x65, 0, 0);
    lo_saved = true;
    if (!lo_select(false, &s->lo_active)) { s->error = ESP_ERR_INVALID_STATE; goto done; }
    set_chanfreq(2412, 0);
    phy_set_freq(s->tuning.pll_khz / 1000U, s->tuning.pll_khz % 1000U);
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    s->gain_max = (REG_READ(AGC_FORCE) >> 8) & 0x7f;
    if (r->gain > s->gain_max) { s->error = ESP_ERR_INVALID_ARG; goto done; }
    force_rx_gain(1, r->gain, 0);
    s->lo_applied = lo_select(s->tuning.divided, &s->lo_active);
    if (!s->lo_applied) { s->error = ESP_ERR_INVALID_STATE; goto done; }
    vTaskDelay(pdMS_TO_TICKS(3));
    if (r->adc) adc_run(r);
    else s->error = collect(r);

done:
    if (r->spill) { s->cleanup_error = rx_spill_close(r->spill); r->spill = NULL; }
    if (rf_setup) {
        REG_WRITE(DUMP_CTRL, 0);
        REG_WRITE(DAC_CTRL, 0);
        REG_CLR_BIT(FE_CTRL, 1U << 1);
        REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(0);
        rom_set_rxclk_en(0);
    }
    if (lo_saved) {
        s->lo_restore_ok = lo_select((s->lo_saved & 0x10U) != 0, &s->lo_restored);
        if (!s->lo_restore_ok && s->cleanup_error == ESP_OK)
            s->cleanup_error = ESP_ERR_INVALID_STATE;
    }
    if (started) {
        esp_err_t stopped = esp_wifi_set_promiscuous(false);
        if (s->cleanup_error == ESP_OK) s->cleanup_error = stopped;
    }
    if (initialized) {
        esp_err_t stopped = esp_wifi_stop();
        if (stopped != ESP_OK && stopped != ESP_ERR_WIFI_NOT_STARTED &&
            s->cleanup_error == ESP_OK) s->cleanup_error = stopped;
        esp_err_t deinitialized = esp_wifi_deinit();
        if (deinitialized != ESP_OK && s->cleanup_error == ESP_OK)
            s->cleanup_error = deinitialized;
    }
    if (s->error == ESP_OK) s->error = s->cleanup_error;
    if (!r->probe && s->retained_pairs) {
        if (!r->pattern && s->retained_pairs >= CONTROL_PAIRS) {
            memcpy(r->snapshot, r->record, 4U * CONTROL_PAIRS);
            s->snapshot_pairs = CONTROL_PAIRS;
        }
        uint32_t before = esp_cpu_get_cycle_count();
        if (!rx_compact((uint32_t *)r->record, s->retained_pairs)) {
            ++s->sentinel_errors;
            s->error = ESP_FAIL;
        }
        s->compact_cycles = esp_cpu_get_cycle_count() - before;
    }
    s->crc32 = esp_rom_crc32_le(0, r->record,
                               r->probe ? s->probe.bytes : 2U * s->retained_pairs);
    if (s->snapshot_pairs)
        s->snapshot_crc32 = esp_rom_crc32_le(0, (uint8_t *)r->snapshot, 4U * s->snapshot_pairs);
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static esp_err_t execute(run *r)
{
    StaticSemaphore_t storage;
    r->done = xSemaphoreCreateBinaryStatic(&storage);
    if (xTaskCreatePinnedToCore(worker, "capture", 8192, r, configMAX_PRIORITIES - 1,
                                NULL, 1) != pdPASS)
        return r->report->error = ESP_ERR_NO_MEM;
    xSemaphoreTake(r->done, portMAX_DELAY);
    return r->report->error;
}

esp_err_t rx_capture(uint16_t mhz, uint8_t gain, uint8_t *record, uint32_t *snapshot,
                     rx_report *report, bool probe)
{
    memset(report, 0, sizeof *report);
    if (mhz < RX_FREQ_MIN_MHZ || mhz > RX_FREQ_MAX_MHZ) return report->error = ESP_ERR_INVALID_ARG;
    run r = {.record = record, .snapshot = snapshot, .report = report,
             .mhz = mhz, .gain = gain, .probe = probe};
    return execute(&r);
}

void rx_pattern(uint8_t *record, rx_report *report)
{
    /* Radio-off raw-copy/compact path, different every complex pair. */
    memset(report, 0, sizeof *report);
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT) {
        report->error = ESP_ERR_INVALID_STATE;
        return;
    }
    run r = {.record = record, .report = report, .pattern = true};
    (void)execute(&r);
}

esp_err_t rx_memory(uint8_t *record, rx_memory_report *report, unsigned dma_burst)
{
    memset(report, 0, sizeof *report);
    if (dma_burst && dma_burst != 16 && dma_burst != 32 && dma_burst != 64)
        return ESP_ERR_INVALID_ARG;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT) return ESP_ERR_INVALID_STATE;
    rx_report capture = {0};
    run r = {.record = record, .report = &capture, .memory = report, .dma_burst = dma_burst};
    return execute(&r);
}

esp_err_t rx_adc(uint16_t mhz, uint8_t gain, uint8_t *record, rx_adc_report *report)
{
    memset(report, 0, sizeof *report);
    if (mhz < 2390 || mhz > 2450) return ESP_ERR_INVALID_ARG;
    rx_report capture = {0};
    run r = {.record = record, .report = &capture, .mhz = mhz, .gain = gain, .adc = report};
    esp_err_t error = execute(&r);
    report->cleanup_error = capture.cleanup_error;
    return error;
}
