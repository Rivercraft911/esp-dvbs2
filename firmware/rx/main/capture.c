/* Independent finite RX experiment. Register facts are documented at the pinned
 * URL in samples.c, sections "Dump and DAC engine", "SRAM bank ownership" and
 * "Vendor library functions as used". The capture engine, bank handoff bounds,
 * 16 MS/s rate and PSRAM deadline are NOT qualified until measured on a board.
 */
#include <string.h>
#include "capture.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_private/regi2c_ctrl.h"
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
    bool probe;
    SemaphoreHandle_t done;
} run;

static uint32_t *IRAM_ATTR bank(unsigned n)
{
    return (uint32_t *)(0x3fcb0000U + n * 65536U);
}

static void IRAM_ATTR clear_bank(unsigned n)
{
    uint32_t *p = bank(n);
    for (unsigned i = 0; i < RX_BANK_PAIRS; ++i) p[i] = RX_SENTINEL;
    __asm__ volatile ("memw" ::: "memory");
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

static esp_err_t IRAM_ATTR collect(run *r)
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

    /* Capture is only ~101 ms. Keep the Wi-Fi MAC ISR and scheduler out of the
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
    uint32_t target = UNIT_PAIRS, refresh = epoch;
    uint16_t expected = 0;
    unsigned current = 0;
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

        uint32_t copy_start = esp_cpu_get_cycle_count();
        if (!s->retained_pairs) {
            for (unsigned n = 0; n < CONTROL_PAIRS; ++n)
                r->snapshot[n] = src[(first + n) & RX_BANK_MASK];
            s->snapshot_pairs = CONTROL_PAIRS;
        }
        if (!rx_pack(src, first, keep, r->record + 2U * s->retained_pairs)) {
            ++s->sentinel_errors;
            break;
        }
        s->retained_pairs += keep;
        clear_bank(current);
        uint32_t copy_cycles = esp_cpu_get_cycle_count() - copy_start;
        if (copy_cycles > s->max_copy_cycles) s->max_copy_cycles = copy_cycles;
        if (s->retained_pairs == RX_RECORD_PAIRS) { error = ESP_OK; break; }

        /* Vendor RX gain can drift during an extended capture. Refresh at bank
         * boundaries and retain the resulting worst-case poll/late measurements. */
        if (esp_cpu_get_cycle_count() - refresh >= 4800000U) {
            force_rx_gain(1, r->gain, 0);
            refresh = esp_cpu_get_cycle_count();
            ++s->gain_refreshes;
        }
        begin = end;
        current = next;
        target += UNIT_PAIRS;
    }
    REG_WRITE(DUMP_CTRL, 0);
    if (!s->engine_cycles) s->engine_cycles = esp_cpu_get_cycle_count() - epoch;
    REG_WRITE(BANK_USAGE, ownership);
    s->elapsed_cycles = esp_cpu_get_cycle_count() - epoch;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return error;
}

static void worker(void *arg)
{
    run *r = arg;
    rx_report *s = r->report;
    bool initialized = false, started = false, rf_setup = false;
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
    regi2c_ctrl_write_reg_mask(0x65, 0, 0, 4, 4, 0);
    set_chanfreq(2412, 0);
    phy_set_freq(r->mhz, 0);
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    s->gain_max = (REG_READ(AGC_FORCE) >> 8) & 0x7f;
    if (r->gain > s->gain_max) { s->error = ESP_ERR_INVALID_ARG; goto done; }
    force_rx_gain(1, r->gain, 0);
    vTaskDelay(pdMS_TO_TICKS(3));
    s->error = collect(r);

done:
    if (rf_setup) {
        REG_WRITE(DUMP_CTRL, 0);
        REG_WRITE(DAC_CTRL, 0);
        REG_CLR_BIT(FE_CTRL, 1U << 1);
        REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(0);
        rom_set_rxclk_en(0);
    }
    if (started) s->cleanup_error = esp_wifi_set_promiscuous(false);
    if (initialized) {
        esp_err_t stopped = esp_wifi_stop();
        if (stopped != ESP_OK && stopped != ESP_ERR_WIFI_NOT_STARTED &&
            s->cleanup_error == ESP_OK) s->cleanup_error = stopped;
        esp_err_t deinitialized = esp_wifi_deinit();
        if (deinitialized != ESP_OK && s->cleanup_error == ESP_OK)
            s->cleanup_error = deinitialized;
    }
    if (s->error == ESP_OK) s->error = s->cleanup_error;
    s->crc32 = esp_rom_crc32_le(0, r->record,
                               r->probe ? s->probe.bytes : 2U * s->retained_pairs);
    s->snapshot_crc32 = esp_rom_crc32_le(0, (uint8_t *)r->snapshot, 4U * s->snapshot_pairs);
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

esp_err_t rx_capture(uint16_t mhz, uint8_t gain, uint8_t *record, uint32_t *snapshot,
                     rx_report *report, bool probe)
{
    memset(report, 0, sizeof *report);
    if (mhz < 2390 || mhz > 2450) return report->error = ESP_ERR_INVALID_ARG;
    StaticSemaphore_t storage;
    run r = {.record = record, .snapshot = snapshot, .report = report,
             .mhz = mhz, .gain = gain, .probe = probe,
             .done = xSemaphoreCreateBinaryStatic(&storage)};
    if (xTaskCreatePinnedToCore(worker, "capture", 8192, &r, configMAX_PRIORITIES - 1,
                                NULL, 1) != pdPASS)
        return report->error = ESP_ERR_NO_MEM;
    xSemaphoreTake(r.done, portMAX_DELAY);
    return report->error;
}

void rx_pattern(uint8_t *record, rx_report *report)
{
    /* Radio-off memory/packing path, deliberately different every complex pair. */
    memset(report, 0, sizeof *report);
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT) {
        report->error = ESP_ERR_INVALID_STATE;
        return;
    }
    uint32_t epoch = esp_cpu_get_cycle_count();
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
    __asm__ volatile ("memw" ::: "memory");
    size_t total = 0;
    for (unsigned unit = 0; total < RX_RECORD_PAIRS; ++unit) {
        unsigned first = (unit * UNIT_PAIRS) & RX_BANK_MASK;
        size_t count = RX_RECORD_PAIRS - total;
        if (count > UNIT_PAIRS) count = UNIT_PAIRS;
        for (size_t n = 0; n < count; ++n) {
            uint32_t at = total + n;
            bank(0)[(first + n) & RX_BANK_MASK] = ((at * 13U) & 1023U) << 10 |
                                                 ((at * 29U + 7U) & 1023U);
        }
        uint32_t before = esp_cpu_get_cycle_count();
        if (!rx_pack(bank(0), first, count, record + 2U * total)) {
            report->error = ESP_FAIL;
            return;
        }
        clear_bank(0);
        uint32_t elapsed = esp_cpu_get_cycle_count() - before;
        if (elapsed > report->max_copy_cycles) report->max_copy_cycles = elapsed;
        total += count;
    }
    report->elapsed_cycles = esp_cpu_get_cycle_count() - epoch;
    report->retained_pairs = total;
    report->crc32 = esp_rom_crc32_le(0, record, RX_RECORD_BYTES);
    report->error = ESP_OK;
}
