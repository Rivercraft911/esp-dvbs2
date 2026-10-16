// Independent ESP32-S3 DAC-bank experiment. This file contains no prior-art code.
//
// Register facts, not an RF qualification. Pinned source: cyber-murmel/esp-sdr-zephyr
// 3214c16a7c9f136414cc5ae9d245ea290c98a6ce, doc/undocumented-registers-esp32s3.md:
// https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md
//   257-306: DAC 0x60033d64, RUN31, held trigger19, count13:0 written directly;
//            rate15=0 is 40 MS/s; zero stops; 0x60033d5c cleared on cleanup.
//   343-383: ownership 0x600c101c[3:0], one-hot banks at 0x3fcb0000+n*64KiB;
//            CPU cannot access an owned bank, changing selection retains offset;
//            physical bank3 contains ROM data and must never be granted.
//   455-478: 0x60006000 force-TX1, base-index25:18, gain-index17:10;
//            force_tx_gain writes slot0, so require base-index0 before keying.
//   1089-1121: analog 0x65:0[4] selects LO=PLL*5/6; clear for 2390..2450 MHz.
//   1355-1365: TX I[9:0], Q[19:10], signed10; RX has the opposite order.
//   1377-1386: vendor tuning/gain/chain entry points; Wi-Fi NULL/promiscuous setup.
// Tone override 0x60006040[18]: jochenhammes/esp32-sdr-trx e1aebbf2ef508c7199a9b6c6c277b57fe0b51aa9,
// docs/research/TX-RESEARCH.md:419-456 (independent reported RF reproduction).
// Overlap geometry: esp-dvbs2-research/A-rf-path.md:70-90, citing pinned ZDAC
// lines287-330: N16380, lead64, tail2048, step14268. This implementation derives
// sample placement and deadlines directly from those numbers. Count convention,
// I/Q sign and continuity still require an external capture on this board.
// Gain codes independently checked in installed IDF v6.1 libphy.a(phy_tx_gain.o)
// .rodata: gain bytes0x04..0x15 and baseband uint16s0x16..0x39. They are hardware
// settings, not copied software or a calibrated dBm table. Source doc:1383.

#include <stdbool.h>
#include <string.h>

#include "radio_s3.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "radio_s3 requires ESP32-S3"
#endif
#if !CONFIG_ESP_PHY_ENABLE_CERT_TEST
#error "radio_s3 needs CONFIG_ESP_PHY_ENABLE_CERT_TEST for vendor gain/key symbols"
#endif
#if CONFIG_FREERTOS_UNICORE
#error "radio_s3 reserves core 1 for timed DAC streaming"
#endif

#define DAC_CTRL 0x60033d64U
#define ADC_CTRL 0x60033d5cU
#define BANK_USAGE 0x600c101cU
#define FE_CTRL 0x60006000U
#define TONE_CTRL 0x60006040U
#define DAC_RUN (1U << 31)
#define DAC_LOOP (1U << 19)
#define CYCLES_PER_SAMPLE 6U
#define STEP_CYCLES (RADIO_S3_STEP_WORDS * CYCLES_PER_SAMPLE)
#define TAIL_LIMIT_CYCLES ((RADIO_S3_TAIL_WORDS - RADIO_S3_LEAD_WORDS) * CYCLES_PER_SAMPLE)
#define EARLY_SWITCH_CYCLES ((RADIO_S3_LEAD_WORDS / 2) * CYCLES_PER_SAMPLE)

/* Reserve full physical banks, including the unused last four words. The linker
 * assertions also exclude static DRAM and IRAM aliases. This table is consumed
 * before the heap and task stacks are created. Startup may use the SRAM before
 * app_main; the DAC is not enabled until a console-requested run.
 * IDF v6.1 heap/port/esp32s3/memory_layout.c:90-97,123-140; heap_memory_layout.h:66.
 */
SOC_RESERVE_MEMORY_REGION(0x3fcc0000, 0x3fce0000, radio_dac_banks);

/* Undocumented vendor ABIs, checked against the installed S3 archives and the
 * pinned register document:1377-1386. Public Wi-Fi APIs own PHY lifetime.
 */
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned int en);
extern void rom_pbus_xpd_tx_on(unsigned int en);
extern void rom_pbus_xpd_tx_off(void);
extern void set_chanfreq(uint16_t mhz, uint8_t bandwidth);
extern void phy_set_freq(uint16_t mhz, int16_t khz);
extern void force_tx_gain(uint16_t gain, uint16_t baseband, uint8_t digital);
extern void force_txon_mode(uint8_t enable, uint8_t mode, uint8_t offset);

static const uint8_t gain_codes[] = {
    0, 16, 32, 48, 49, 81, 82, 115, 87, 119, 107, 95, 111, 127, 127, 127, 127, 127,
};

static uint16_t gain_baseband(unsigned gain)
{
    /* The first fourteen vendor settings have no baseband boost. */
    static const uint16_t boost[] = {0x080, 0x100, 0x020, 0x0a0};
    return gain < 14 ? 0 : boost[gain - 14];
}

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool running;
static StaticSemaphore_t done_storage;

typedef struct {
    uint32_t freq_mhz;
    uint32_t seconds;
    uint8_t gain;
    radio_s3_fill_fn fill;
    void *ctx;
    radio_s3_stats_t *stats;
    SemaphoreHandle_t done;
    esp_err_t error;
    bool watch_worker;
} run_t;

static inline uint32_t IRAM_ATTR cycles(void)
{
    return esp_cpu_get_cycle_count();
}

static inline void IRAM_ATTR wait_until(uint32_t deadline)
{
    while ((int32_t)(cycles() - deadline) < 0) {
        __asm__ volatile ("nop");
    }
}

static void direct_lo(void)
{
    /* regi2c_ctrl_write_reg_mask serializes with the PHY's analog-I2C lock. */
    regi2c_ctrl_write_reg_mask(0x65, 0, 0, 4, 4, 0);
}

static esp_err_t IRAM_ATTR fill_bank(run_t *run, unsigned bank, uint64_t start)
{
    uint32_t *base = (uint32_t *)(bank == 0 ? 0x3fcd0000 : 0x3fcc0000);
    size_t offset = start % RADIO_S3_BANK_WORDS;
    size_t first_count = RADIO_S3_BANK_WORDS - offset;
    uint32_t before = cycles();
    esp_err_t error = run->fill(base + offset, start, first_count, run->ctx);
    if (error == ESP_OK && offset) {
        error = run->fill(base, start + first_count, offset, run->ctx);
    }
    __asm__ volatile ("memw" ::: "memory");
    uint32_t elapsed = cycles() - before;
    run->stats->fills++;
    if (elapsed > run->stats->max_fill_cycles) {
        run->stats->max_fill_cycles = elapsed;
    }
    return error;
}

/* Return timing only against the original DAC epoch. Never reset the engine or
 * move its epoch to conceal a missed switch. Aborting exposes the first fault.
 */
static esp_err_t IRAM_ATTR __attribute__((noinline)) stream(run_t *run)
{
    radio_s3_stats_t *stats = run->stats;
    uint32_t ownership = REG_READ(BANK_USAGE) & ~0xfU;
    uint64_t total_samples = (uint64_t)run->seconds * RADIO_S3_DAC_RATE;
    esp_err_t error = fill_bank(run, 0, 0);
    if (error != ESP_OK) {
        return error;
    }
    error = fill_bank(run, 1, RADIO_S3_STEP_WORDS - RADIO_S3_LEAD_WORDS);
    if (error != ESP_OK) {
        return error;
    }

    /* Slot0 is the one force_tx_gain programmed. Refuse an unexpected vendor
     * base index rather than quietly keying with an unrelated power setting.
     */
    if (((REG_READ(FE_CTRL) >> 18) & 0xffU) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    rom_pbus_workmode();
    rom_pbus_xpd_rx_on(0);
    rom_pbus_xpd_tx_on(1);
    force_tx_gain(gain_codes[run->gain], gain_baseband(run->gain), 0);
    direct_lo();
    force_txon_mode(1, 0, 0);
    REG_CLR_BIT(TONE_CTRL, 1U << 18);
    /* Keying can clear bank ownership, so grant only after keying. */
    REG_WRITE(BANK_USAGE, ownership | (1U << 2));
    REG_WRITE(DAC_CTRL, DAC_RUN | RADIO_S3_BANK_WORDS);
    uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t epoch = cycles();
    REG_WRITE(DAC_CTRL, DAC_RUN | DAC_LOOP | RADIO_S3_BANK_WORDS);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);

    uint32_t previous = epoch;
    uint32_t target = epoch + STEP_CYCLES;
    uint64_t handoff = RADIO_S3_STEP_WORDS;
    unsigned next = 1;
    for (;;) {
        uint32_t now = cycles();
        stats->elapsed_cycles += (uint32_t)(now - previous);
        previous = now;
        if (handoff >= total_samples) {
            /* At most one bank interval remains, so the uint32 deadline is safe. */
            uint32_t end = target - (uint32_t)((handoff - total_samples) * CYCLES_PER_SAMPLE);
            wait_until(end);
            break;
        }

        int32_t fill_late = (int32_t)(now - target);
        if (fill_late > 0) {
            stats->late_fills++;
        } else if ((uint32_t)-fill_late < stats->min_fill_slack_cycles) {
            stats->min_fill_slack_cycles = (uint32_t)-fill_late;
        }
        if (fill_late >= (int32_t)TAIL_LIMIT_CYCLES) {
            stats->fatal_late++;
            error = ESP_ERR_TIMEOUT;
            break;
        }

        /* Half the lead is deliberate timing margin: aim 32 samples early;
         * both banks contain the same samples for all 64 lead samples. Mask
         * interrupts only for the final wait and register write (about 0.8 us).
         */
        wait_until(target - EARLY_SWITCH_CYCLES - 128);
        irq = portSET_INTERRUPT_MASK_FROM_ISR();
        wait_until(target - EARLY_SWITCH_CYCLES);
        now = cycles();
        if ((int32_t)(now - target) >= (int32_t)TAIL_LIMIT_CYCLES) {
            portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
            stats->fatal_late++;
            error = ESP_ERR_TIMEOUT;
            break;
        }
        REG_WRITE(BANK_USAGE, ownership | (next == 0 ? (1U << 2) : (1U << 1)));
        now = cycles();
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
        stats->switches++;
        int32_t late = (int32_t)(now - target);
        if (late > 0) {
            stats->late_switches++;
            if ((uint32_t)late > stats->max_switch_late_cycles) {
                stats->max_switch_late_cycles = late;
            }
        }
        stats->elapsed_cycles += (uint32_t)(now - previous);
        previous = now;
        if (run->watch_worker) {
            (void)esp_task_wdt_reset();
        }
        next ^= 1;
        handoff += RADIO_S3_STEP_WORDS;
        target += STEP_CYCLES;
        if (handoff < total_samples) {
            error = fill_bank(run, next, handoff - RADIO_S3_LEAD_WORDS);
            if (error != ESP_OK) {
                break;
            }
        }
    }
    /* Halt before any noncritical cleanup, including task-watchdog calls. */
    REG_WRITE(DAC_CTRL, 0);
    uint32_t now = cycles();
    /* A long interrupt can also postpone the final stop past the active
     * bank's overlap. Expose that fault even though no further switch follows.
     */
    if (error == ESP_OK && (int32_t)(now - target) >= (int32_t)TAIL_LIMIT_CYCLES) {
        stats->fatal_late++;
        error = ESP_ERR_TIMEOUT;
    }
    stats->elapsed_cycles += (uint32_t)(now - previous);
    stats->elapsed_samples = stats->elapsed_cycles / CYCLES_PER_SAMPLE;
    return error;
}

static void worker(void *arg)
{
    run_t *run = arg;
    bool initialized = false;
    bool started = false;
    bool watch_worker = false;
    bool watch_idle = false;
    TaskHandle_t idle = xTaskGetIdleTaskHandleForCore(1);
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t cpu_lock = NULL;
    esp_pm_lock_handle_t sleep_lock = NULL;
#endif
    wifi_mode_t existing;
    esp_err_t error = esp_wifi_get_mode(&existing);
    if (error != ESP_ERR_WIFI_NOT_INIT) {
        error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
#if CONFIG_PM_ENABLE
    error = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "radio_cpu", &cpu_lock);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = esp_pm_lock_acquire(cpu_lock);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "radio_awake", &sleep_lock);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = esp_pm_lock_acquire(sleep_lock);
    if (error != ESP_OK) {
        goto cleanup;
    }
#endif
    if (esp_clk_cpu_freq() != 240000000) {
        error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0; /* Wi-Fi configuration stays in RAM. */
    error = esp_wifi_init(&config);
    if (error != ESP_OK) {
        goto cleanup;
    }
    initialized = true;
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_NULL);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        goto cleanup;
    }
    started = true;
    error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error == ESP_OK) {
        error = esp_wifi_set_promiscuous(true);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    }
    if (error != ESP_OK) {
        goto cleanup;
    }
    REG_WRITE(DAC_CTRL, 0);
    REG_WRITE(ADC_CTRL, 0);
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~0xfU);
    force_txon_mode(0, 0, 0);
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(0);
    direct_lo();
    set_chanfreq(2412, 2); /* Real Wi-Fi channel, fixed wide (40 MHz) filter mode. */
    phy_set_freq(run->freq_mhz, 0);
    /* No MAC frames are transmitted in NULL mode. Allow retune to settle while
     * TX remains unkeyed. Mode2 passes more tag bandwidth; its image rejection
     * and whether the 40 Mchip/s tag survives remain bench questions.
     */
    vTaskDelay(pdMS_TO_TICKS(3));

    /* Streaming occupies core1 without an idle tick. Transfer that core's
     * watchdog subscription to the producer, then restore it on every exit.
     * A stuck fill/switch still trips the task watchdog.
     */
    if (esp_task_wdt_status(idle) == ESP_OK) {
        error = esp_task_wdt_delete(idle);
        if (error != ESP_OK) {
            goto cleanup;
        }
        watch_idle = true;
    }
    error = esp_task_wdt_add(NULL);
    if (error == ESP_OK) {
        watch_worker = true;
        run->watch_worker = true;
    } else if (error != ESP_ERR_INVALID_STATE) {
        goto cleanup;
    }
    error = stream(run);

cleanup:
    if (started) {
        REG_WRITE(DAC_CTRL, 0);
        REG_WRITE(ADC_CTRL, 0);
        force_txon_mode(0, 0, 0);
        REG_CLR_BIT(FE_CTRL, 1U << 1);
        REG_CLR_BIT(TONE_CTRL, 1U << 18);
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(0);
        REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~0xfU);
        (void)esp_wifi_set_promiscuous(false);
    }
    if (initialized) {
        /* Stop even when esp_wifi_start returned an error after partial setup. */
        esp_err_t stopped = esp_wifi_stop();
        if (error == ESP_OK && stopped != ESP_OK) {
            error = stopped;
        }
        esp_err_t deinitialized = esp_wifi_deinit();
        if (error == ESP_OK && deinitialized != ESP_OK) {
            error = deinitialized;
        }
    }
    if (watch_worker) {
        (void)esp_task_wdt_delete(NULL);
    }
    if (watch_idle) {
        (void)esp_task_wdt_add(idle);
    }
#if CONFIG_PM_ENABLE
    if (sleep_lock) {
        (void)esp_pm_lock_release(sleep_lock);
        (void)esp_pm_lock_delete(sleep_lock);
    }
    if (cpu_lock) {
        (void)esp_pm_lock_release(cpu_lock);
        (void)esp_pm_lock_delete(cpu_lock);
    }
#endif
    run->error = error;
    xSemaphoreGive(run->done);
    vTaskDelete(NULL);
}

esp_err_t radio_s3_run(uint32_t freq_mhz, uint8_t gain, uint32_t seconds,
                       radio_s3_fill_fn fill, void *ctx, radio_s3_stats_t *stats)
{
    if (!fill || !stats || freq_mhz < 2390 || freq_mhz > 2450 ||
            gain > RADIO_S3_GAIN_MAX || seconds == 0 || seconds > 60) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&state_lock);
    if (running) {
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    running = true;
    portEXIT_CRITICAL(&state_lock);
    memset(stats, 0, sizeof *stats);
    stats->min_fill_slack_cycles = UINT32_MAX;
    run_t run = {
        .freq_mhz = freq_mhz, .gain = gain, .seconds = seconds,
        .fill = fill, .ctx = ctx, .stats = stats,
        .done = xSemaphoreCreateBinaryStatic(&done_storage),
    };
    BaseType_t created = xTaskCreatePinnedToCore(worker, "radio", 6144, &run,
                                                configMAX_PRIORITIES - 2, NULL, 1);
    if (created == pdPASS) {
        xSemaphoreTake(run.done, portMAX_DELAY);
    } else {
        run.error = ESP_ERR_NO_MEM;
    }
    if (stats->min_fill_slack_cycles == UINT32_MAX) {
        stats->min_fill_slack_cycles = 0;
    }
    portENTER_CRITICAL(&state_lock);
    running = false;
    portEXIT_CRITICAL(&state_lock);
    return run.error;
}
