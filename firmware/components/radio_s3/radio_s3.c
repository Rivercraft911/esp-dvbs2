// ESP32-S3 transmit streaming through two alternating DAC SRAM banks.
// Register reference: cyber-murmel/esp-sdr-zephyr
// 3214c16a7c9f136414cc5ae9d245ea290c98a6ce, doc/undocumented-registers-esp32s3.md:
// https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md
//   257-306: DAC 0x60033d64, RUN31, held trigger19, count13:0 written directly;
//            rate15=0 is 40 MS/s; zero stops; 0x60033d5c cleared on cleanup.
//   343-383: ownership 0x600c101c[3:0], one-hot banks at 0x3fcb0000+n*64KiB;
//            CPU cannot access an owned bank, changing selection retains offset;
//            physical bank3 contains ROM data and must never be granted.
//   455-478: 0x60006000 force-TX1, base-index25:18, gain-index17:10;
//            force_tx_gain writes slot0; the keying offset cancels the base index.
//   1089-1121: analog 0x65:0[4] selects LO=PLL*5/6; clear for direct LO.
//   1355-1365: TX I[9:0], Q[19:10], signed10; RX has the opposite order.
//   1377-1386: vendor tuning/gain/chain entry points.
// Tone override 0x60006040[18]: jochenhammes/esp32-sdr-trx
// e1aebbf2ef508c7199a9b6c6c277b57fe0b51aa9, docs/research/TX-RESEARCH.md:419-456.
// At 40 MS/s, each bank has 64 lead and 2048 tail samples of overlap, leaving
// 14268 new samples. radio_s3.h defines the geometry and timing for both rates.
// Gain codes come from IDF v6.1 libphy.a(phy_tx_gain.o) .rodata:
// gain bytes0x04..0x15 and baseband uint16s0x16..0x39. Output power is uncalibrated.
// See docs/prior-art.md for source links and docs/measurements.md for RF results.

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio_s3.h"
#include "radio_power.h"
#include "esp_timer.h"
#include "esp_private/phy.h"
#include "esp_private/periph_ctrl.h"
#include "esp_phy_init.h"
#include "station_id.h"
#include "station_store.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/extmem_reg.h"
#include "sdkconfig.h"
#include "hal/systimer_ll.h"
#include "esp_private/systimer.h"
extern void esp_vApplicationTickHook(void); /* esp_system/freertos_hooks.c */

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
#define DAC_RATE_80M (1U << 15) /* pinned register document:257-306, "DUMP_DAC_RATE_80M" */

/* A run's bank geometry (radio_s3.h) and DAC rate, at 240 MHz. Copied into the run so that
 * the schedule reads it from internal RAM. */
typedef struct {
    uint32_t rate;              /* samples per second */
    uint32_t rate_bit;          /* DAC_CTRL */
    uint32_t cycles;            /* CPU cycles per sample */
    uint32_t lead, tail, step;  /* words */
} geometry_t;

static geometry_t geometry(uint32_t dac_rate)
{
    if (dac_rate == RADIO_S3_DAC_RATE_80M) {
        return (geometry_t){dac_rate, DAC_RATE_80M, 3, RADIO_S3_LEAD_WORDS_80M,
                            RADIO_S3_TAIL_WORDS_80M, RADIO_S3_STEP_WORDS_80M};
    }
    return (geometry_t){RADIO_S3_DAC_RATE, 0, 6, RADIO_S3_LEAD_WORDS, RADIO_S3_TAIL_WORDS,
                        RADIO_S3_STEP_WORDS};
}

/* Reserve full physical banks, including the unused last four words. The linker
 * assertions also exclude static DRAM and IRAM aliases. This table is consumed
 * before the heap and task stacks are created. Startup may use the SRAM before
 * app_main; the DAC is not enabled until a console-requested run.
 * IDF v6.1 heap/port/esp32s3/memory_layout.c:90-97,123-140; heap_memory_layout.h:66.
 */
SOC_RESERVE_MEMORY_REGION(0x3fcc0000, 0x3fce0000, radio_dac_banks);

/* Undocumented vendor ABIs, checked against the installed S3 archives and the
 * pinned register document:1377-1386. radio_s3_init owns the PHY lifetime.
 */
extern void rom_pbus_xpd_rx_on(unsigned int en);
extern void rom_pbus_xpd_tx_off(void);
extern void set_chanfreq(uint16_t mhz, uint8_t bandwidth);
extern void phy_set_freq(uint16_t mhz, int16_t khz);
extern void force_tx_gain(uint16_t gain, uint16_t baseband, uint8_t digital);
extern void force_txon_mode(uint8_t enable, uint8_t mode, uint8_t offset);
extern void stop_tx_tone(unsigned int enable);
extern void esp_phy_wifi_tx_tone(uint32_t start, uint32_t chan, uint32_t backoff);
extern void esp_phy_cbw40m_en(bool en);

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
static bool lo_divided;
static int32_t lo_offset_khz;

void radio_s3_lo_command(const char *args)
{
    char mode[16], *end;
    int at = 0;
    if (sscanf(args, "%15s%n", mode, &at) != 1) goto usage;
    const bool divided = !strcmp(mode, "five-sixths");
    if (!divided && strcmp(mode, "direct")) goto usage;
    const char *value = args + at;
    while (*value == ' ' || *value == '\t') ++value;
    long offset = 0;
    if (*value) {
        errno = 0;
        offset = strtol(value, &end, 10);
        if (errno || end == value || offset < -999 || offset > 999) goto usage;
        while (*end == ' ' || *end == '\t') ++end;
        if (*end) goto usage;
    }
    lo_divided = divided;
    lo_offset_khz = offset;
    printf("@{\"cmd\":\"rflo\",\"mode\":\"%s\",\"offset_khz\":%ld}\n", mode, offset);
    return;
usage:
    printf("@{\"cmd\":\"rflo\",\"error\":\"usage: rflo direct|five-sixths [-999..999 offset kHz]\"}\n");
}

static uint32_t requested_rf_khz(uint32_t mhz)
{
    return (uint32_t)((int64_t)mhz * 1000 + lo_offset_khz);
}

void radio_s3_tuning_print(const radio_s3_stats_t *stats)
{
    printf(",\"requested_rf_khz\":%lu,\"lo_mode\":\"%s\",\"lo_offset_khz\":%ld,"
           "\"requested_pll_khz\":%lu", (unsigned long)stats->requested_rf_khz,
           stats->lo_divided ? "five-sixths" : "direct", (long)stats->lo_offset_khz,
           (unsigned long)stats->requested_pll_khz);
}

/* Keep this cold setup out of the renderer's fixed IRAM literal layout. */
static __attribute__((noinline)) void tune(uint32_t mhz)
{
    const uint32_t rf = requested_rf_khz(mhz);
    /* Nearest integer PLL kHz. Integer-MHz requests remain exact. */
    const uint32_t khz = lo_divided ? (6U * rf + 2U) / 5U : rf;
    phy_set_freq(khz / 1000, khz % 1000);
    regi2c_ctrl_write_reg_mask(0x65, 0, 0, 4, 4, lo_divided);
}

/* Nearest 2.4 GHz Wi-Fi channel, for the vendor CW call (channels 1..13). */
static uint32_t cw_channel(uint32_t mhz)
{
    int chan = ((int)mhz - 2407 + 2) / 5;
    return chan < 1 ? 1 : chan > 13 ? 13 : (uint32_t)chan;
}

/* Vendor CW backoff in 0.25 dB: gain 17 is full power, each step 2 dB less. */
static uint32_t cw_backoff(unsigned gain)
{
    return (RADIO_S3_GAIN_MAX - gain) * 8;
}
static StaticSemaphore_t done_storage;
/* One radio worker on core 1 for the life of the firmware, created at boot with a static stack
 * in internal SRAM (a heap stack could land in slow RTC FAST memory). It holds core 1's
 * task-watchdog subscription; core 1's idle task is not checked, since a run never lets it in.
 * Between runs the worker feeds the watchdog once a second; during one, every bank switch does,
 * so a stuck fill or switch still trips it. Nothing is allocated per run. */
static StackType_t worker_stack[6144 / sizeof(StackType_t)];
static StaticTask_t worker_tcb;
static TaskHandle_t worker_task;
static StaticSemaphore_t job_storage;
static SemaphoreHandle_t job_ready;
static TaskFunction_t job_task;
static void *job_arg;
static bool watched;
static station_id_t worker_id;

// Final stream preparation after the worker exists, before either paced deadlines or the
// first keyed register access.
esp_err_t __attribute__((weak)) radio_s3_stream_ready(void) { return ESP_OK; }

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
    uint64_t samples;               /* exact sample count, including IDs */
    bool identify;
    bool limit_duration;
    bool paced;                     /* bank deadlines without the radio */
    radio_s3_sink_fn sink;
    void *sink_ctx;
    radio_s3_dry_stats_t *dry;
    geometry_t geo;
    radio_s3_options_t options;
    radio_s3_startup_t *startup;
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

uint32_t *radio_s3_bank(unsigned n)
{
    return (uint32_t *)(n == 0 ? 0x3fcd0000 : 0x3fcc0000);
}

static esp_err_t IRAM_ATTR fill_bank(run_t *run, unsigned bank, uint64_t start)
{
    uint32_t *base = radio_s3_bank(bank);
    size_t offset = start % RADIO_S3_BANK_WORDS;
    size_t first_count = RADIO_S3_BANK_WORDS - offset;
    radio_s3_startup_t *trace = run->startup;
    // Only the opening ID is traced. Its length is uint32_t, and collection
    // finishes within 32 banks, before the timeline can wrap that range.
    const bool record = trace && trace->count < RADIO_S3_STARTUP_BANKS &&
                        (uint32_t)start >= (uint32_t)trace->first_sample;
    uint32_t trace_before = cycles();
    uint32_t psram = record ? REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG) : 0;
    uint32_t before = cycles();
    esp_err_t error = run->fill(base + offset, start, first_count, run->ctx);
    if (error == ESP_OK && offset) {
        error = run->fill(base, start + first_count, offset, run->ctx);
    }
    __asm__ volatile ("memw" ::: "memory");
    uint32_t elapsed = cycles() - before;
    if (record) {
        const unsigned at = trace->count++;
        trace->bank[at].cycles = elapsed;
        trace->bank[at].psram_miss = REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG) - psram;
        trace->bank[at].trace_cycles = cycles() - trace_before - elapsed;
    }
    run->stats->fills++;
    if (elapsed > run->stats->max_fill_cycles) {
        run->stats->max_fill_cycles = elapsed;
        run->stats->max_fill_sample = start;
    }
    return error;
}

/* The bank schedule from the DAC epoch on, shared by the radio and the paced run.
 * Return timing only against the original DAC epoch. Never reset the engine or
 * move its epoch to conceal a missed switch. Aborting exposes the first fault.
 * Both callers pass a constant `keyed` and the function is inlined into each, so
 * the paced copy contains no register write at all.
 */
static inline __attribute__((always_inline)) esp_err_t schedule(run_t *run, uint32_t epoch,
                                                               uint32_t ownership, bool keyed)
{
    radio_s3_stats_t *stats = run->stats;
    uint64_t total_samples = run->samples;
    const geometry_t g = run->geo;
    const uint32_t step_cycles = g.step * g.cycles;
    /* A late switch must still land before the old bank's tail runs out; LEAD of it is kept
     * back for an unknown DAC read-ahead. */
    const int32_t tail_limit = (int32_t)((g.tail - g.lead) * g.cycles);
    const uint32_t early = g.lead / 2 * g.cycles;
    esp_err_t error = ESP_OK;
    uint32_t irq;
    uint32_t previous = epoch;
    uint32_t target = epoch + step_cycles;
    uint64_t handoff = g.step;
    unsigned next = 1;
    for (;;) {
        uint32_t now = cycles();
        stats->elapsed_cycles += (uint32_t)(now - previous);
        previous = now;
        if (handoff >= total_samples) {
            /* At most one bank interval remains, so the uint32 deadline is safe. */
            uint32_t end = target - (uint32_t)((handoff - total_samples) * g.cycles);
            wait_until(end);
            break;
        }

        int32_t fill_late = (int32_t)(now - target);
        if (fill_late > 0) {
            stats->late_fills++;
        } else if ((uint32_t)-fill_late < stats->min_fill_slack_cycles) {
            stats->min_fill_slack_cycles = (uint32_t)-fill_late;
        }
        if (fill_late >= tail_limit) {
            stats->fatal_late++;
            error = ESP_ERR_TIMEOUT;
            break;
        }

        /* Half the lead is deliberate timing margin: aim LEAD/2 samples early;
         * both banks contain the same samples for all LEAD samples.
         * Mask levels 1-3 from the end of the fill to the switch, so that core 1
         * serves interrupts only while it fills. A switch then lands late only if
         * its fill overran. With the mask over just the last 320 cycles, the 1 kHz
         * tick made 0.3% of switches up to ~750 cycles late (results/2026-10-16-
         * radio); the interrupts move to the next fill, at most one tick per bank.
         */
        irq = portSET_INTERRUPT_MASK_FROM_ISR();
        wait_until(target - early);
        now = cycles();
        if ((int32_t)(now - target) >= tail_limit) {
            portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
            stats->fatal_late++;
            error = ESP_ERR_TIMEOUT;
            break;
        }
        if (keyed) {
            REG_WRITE(BANK_USAGE, ownership | (next == 0 ? (1U << 2) : (1U << 1)));
        }
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
        /* Core 1's tick is off for the run (core1_tick): run its tick hooks here, outside any
         * fill, so the interrupt watchdog's core-1 check (800 ms) still sees core 1 alive.
         * Every 1,024 banks is at most 365 ms. */
        if (!(stats->switches & 1023)) {
            esp_vApplicationTickHook();
        }
        next ^= 1;
        handoff += g.step;
        target += step_cycles;
        if (handoff < total_samples) {
            error = fill_bank(run, next, handoff - g.lead);
            if (error != ESP_OK) {
                break;
            }
        }
    }
    /* Halt before any noncritical cleanup, including task-watchdog calls. */
    if (keyed) {
        REG_WRITE(DAC_CTRL, 0);
    }
    uint32_t now = cycles();
    /* A long interrupt can also postpone the final stop past the active
     * bank's overlap. Expose that fault even though no further switch follows.
     */
    if (error == ESP_OK && (int32_t)(now - target) >= tail_limit) {
        stats->fatal_late++;
        error = ESP_ERR_TIMEOUT;
    }
    stats->elapsed_cycles += (uint32_t)(now - previous);
    stats->elapsed_samples = stats->elapsed_cycles / g.cycles;
    return error;
}

static esp_err_t IRAM_ATTR __attribute__((noinline)) stream(run_t *run)
{
    radio_s3_stats_t *stats = run->stats;
    uint32_t ownership = REG_READ(BANK_USAGE) & ~0xfU;
    esp_err_t error = fill_bank(run, 0, 0);
    if (error != ESP_OK) {
        return error;
    }
    error = fill_bank(run, 1, run->geo.step - run->geo.lead);
    if (error != ESP_OK) {
        return error;
    }

    /* force_tx_gain programs gain memory slot 0, but the front end uses slot
     * (offset + base index) & 0xff (FE_CTRL bits 25:18). ESP-IDF v6.1 leaves a
     * nonzero base index after Wi-Fi start (2026-10-16); cancel any base index with the
     * offset and read the selected slot back before any sample plays. A wrong
     * slot would key at a vendor power setting, so abort; cleanup unkeys.
     */
    const uint32_t base = (REG_READ(FE_CTRL) >> 18) & 0xffU;
    stats->fe_base_index = base;
    /* CW bring-up enables both I/Q branches with calibrated PHY settings.
     * Stop its tone, move only the PLL, then pin gain slot zero. */
    esp_phy_cbw40m_en(false);
    esp_phy_wifi_tx_tone(1, cw_channel(run->freq_mhz), cw_backoff(run->gain));
    stop_tx_tone(1);
    direct_lo();
    tune(run->freq_mhz);
    force_tx_gain(gain_codes[run->gain], gain_baseband(run->gain), 0);
    force_txon_mode(1, 0, (uint8_t)(-base & 0xffU));
    stats->fe_gain_index = (REG_READ(FE_CTRL) >> 10) & 0xffU;
    if (stats->fe_gain_index != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!radio_filter_apply(&stats->filter)) {
        return ESP_ERR_INVALID_STATE;
    }
    REG_CLR_BIT(TONE_CTRL, 1U << 18);
    /* Keying can clear bank ownership, so grant only after keying. */
    REG_WRITE(BANK_USAGE, ownership | (1U << 2));
    const uint32_t rate = run->geo.rate_bit;
    REG_WRITE(DAC_CTRL, DAC_RUN | rate | RADIO_S3_BANK_WORDS);
    uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t epoch = cycles();
    REG_WRITE(DAC_CTRL, DAC_RUN | DAC_LOOP | rate | RADIO_S3_BANK_WORDS);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return schedule(run, epoch, ownership, true);
}

/* Radio off: the fills of stream() into the same banks, against the same deadlines. */
static esp_err_t IRAM_ATTR __attribute__((noinline)) paced(run_t *run)
{
    esp_err_t error = fill_bank(run, 0, 0);
    if (error == ESP_OK) {
        error = fill_bank(run, 1, run->geo.step - run->geo.lead);
    }
    if (error != ESP_OK) {
        return error;
    }
    uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t epoch = cycles();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return schedule(run, epoch, 0, false);
}

static void heap_now(uint32_t *free_bytes, uint32_t *largest)
{
    *free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* The radio, once at boot and for good, without Wi-Fi (radio_power.h). The DAC streams from the
 * Wi-Fi baseband, so its clock domain stays referenced. */
static bool phy_up;
extern void phy_param_track_tot(bool en_wifi, bool en_ble_154); /* libphy */

static void core1(void *arg)
{
    (void)arg;
    watched = esp_task_wdt_add(NULL) == ESP_OK;
    for (;;) {
        if (xSemaphoreTake(job_ready, pdMS_TO_TICKS(1000)) == pdTRUE) {
            job_task(job_arg);
        }
        if (watched) {
            (void)esp_task_wdt_reset();
        }
    }
}

esp_err_t radio_s3_init(void)
{
    if (!worker_task) {
        job_ready = xSemaphoreCreateBinaryStatic(&job_storage);
        worker_task = xTaskCreateStaticPinnedToCore(core1, "radio", sizeof worker_stack, NULL,
                                                    configMAX_PRIORITIES - 2, worker_stack,
                                                    &worker_tcb, 1);
    }
    const esp_err_t e = radio_power_up();
    phy_up = e == ESP_OK;
    return worker_task ? e : ESP_ERR_NO_MEM;
}

/* During a run core 1 runs only this worker, so its FreeRTOS tick (time slicing on core 1;
 * core 0's tick keeps time and wakes core-1 tasks) only costs the fills about 1k cycles a
 * bank once a millisecond. Stop that alarm's interrupt for the run. Re-enabled, the handler
 * (freertos/port_systick.c) runs every skipped tick at once: 29 ms after a 10 s pace, 100 ms
 * after 60 s, so about 0.8 s after the 540 s limit, on core 1 only and with the
 * transmitter already off. Nothing else toggles this enable bit after boot. schedule() runs
 * core 1's tick hooks (the interrupt watchdog's) between fills meanwhile. */
static void core1_tick(bool on)
{
    systimer_ll_enable_alarm_int(&SYSTIMER, SYSTIMER_ALARM_OS_TICK_CORE0 + 1, on);
}

static void worker(void *arg)
{
    run_t *run = arg;
    radio_s3_startup_t startup = {0};
    if (run->paced && run->options.startup) {
        const uint64_t end = run->stats->id_samples;
        startup.first_sample = end < RADIO_S3_BANK_WORDS ? 0 :
            ((end+run->geo.lead-RADIO_S3_BANK_WORDS)/run->geo.step+1)*run->geo.step-run->geo.lead;
        run->startup = &startup;
    }
    run->watch_worker = watched;
    bool rf_setup = false, tick_off = false;
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t cpu_lock = NULL;
    esp_pm_lock_handle_t sleep_lock = NULL;
#endif
    esp_err_t error = ESP_OK;
    if (!run->paced && !phy_up) {
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
    error = radio_s3_stream_ready();
    if (error == ESP_OK && run->paced) {
        core1_tick(false);
        tick_off = true;
        error = paced(run);
        goto cleanup;
    }
    if (error != ESP_OK) {
        goto cleanup;
    }
    /* One PLL and temperature tracking pass before keying, as Wi-Fi's 1 s timer used to run.
     * It returns a 16-byte front-end table to its Wi-Fi-up values (the receiver's register diff,
     * results/2026-10-23-rx-no-wifi-ab) and takes about 44,000 cycles. */
    _lock_t phy_lock = phy_get_lock();
    _lock_acquire(&phy_lock);
    phy_param_track_tot(true, false);
    _lock_release(&phy_lock);
    rf_setup = true;
    REG_WRITE(DAC_CTRL, 0);
    REG_WRITE(ADC_CTRL, 0);
    REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~0xfU);
    force_txon_mode(0, 0, 0);
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(0);
    direct_lo();
    set_chanfreq(2412, 0); /* Real Wi-Fi channel. Final filter is applied after CW setup. */
    tune(run->freq_mhz);
    /* Allow the unkeyed retune to settle. */
    vTaskDelay(pdMS_TO_TICKS(3));

    core1_tick(false);
    tick_off = true;
    error = stream(run);

cleanup:
    if (rf_setup) {
        REG_WRITE(DAC_CTRL, 0);
        if (run->stats->filter.applied && !radio_filter_finish(&run->stats->filter) && error == ESP_OK) {
            error = ESP_ERR_INVALID_STATE;
        }
        REG_WRITE(ADC_CTRL, 0);
        esp_phy_wifi_tx_tone(0, cw_channel(run->freq_mhz), cw_backoff(run->gain));
        force_txon_mode(0, 0, 0);
        REG_CLR_BIT(FE_CTRL, 1U << 1);
        REG_CLR_BIT(TONE_CTRL, 1U << 18);
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(0);
        REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~0xfU);
    }
    if (tick_off) {
        core1_tick(true);
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
    if (run->startup) {
        *run->options.startup = startup;
        run->startup = NULL;
    }
    run->error = error;
    xSemaphoreGive(run->done);
}

/* The dry run: the fill sequence of stream() and the samples the DAC would play. */
static void dry_worker(void *arg)
{
    run_t *run = arg;
    radio_s3_dry_stats_t *stats = run->dry;
    const uint64_t samples = run->samples;
    const uint64_t step = run->geo.step, lead = run->geo.lead, tail = run->geo.tail;
    esp_err_t error = fill_bank(run, 0, 0);
    if (error == ESP_OK) {
        error = fill_bank(run, 1, step - lead);
    }
    for (uint64_t j = 0; error == ESP_OK && j * step < samples; ++j) {
        // Feed the watchdog every bank: a dump's USB sink can take 80 ms per bank, and a
        // watchdog report would land in the dump. Let other core-1 work run now and then.
        if (watched) {
            (void)esp_task_wdt_reset();
        }
        if ((j & 63) == 63) {
            vTaskDelay(1);
        }
        const uint32_t *now = radio_s3_bank(j & 1), *next = radio_s3_bank((j + 1) & 1);
        const uint64_t edge = (j + 1) * step;
        for (uint64_t n = edge - lead; n < edge + tail; ++n) {
            const size_t at = n % RADIO_S3_BANK_WORDS;
            stats->overlap_mismatches += now[at] != next[at];
        }
        stats->overlap_words += lead + tail;
        const uint64_t left = samples - j * step;
        error = run->sink(now, j * step, left < step ? (size_t)left : (size_t)step,
                          run->sink_ctx);
        if (error == ESP_OK && (j + 1) * step < samples) {
            error = fill_bank(run, j & 1, (j + 2) * step - lead);
        }
    }
    stats->fills = run->stats->fills;
    stats->max_fill_cycles = run->stats->max_fill_cycles;
    run->error = error;
    xSemaphoreGive(run->done);
}

/* Run one job on the core-1 worker and wait for it. One run at a time. */
static esp_err_t on_core1(TaskFunction_t task, run_t *run)
{
    portENTER_CRITICAL(&state_lock);
    if (running) {
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    running = true;
    portEXIT_CRITICAL(&state_lock);
    station_id_t *id = NULL;
    if (run->identify) {
        id = &worker_id;
        memset(id, 0, sizeof *id);
        if (!station_id_init_rate(id, station_store_get(), run->samples, run->geo.rate,
                                  run->fill, run->ctx)) {
            run->error = ESP_ERR_INVALID_STATE;
            goto finished;
        }
        if (run->limit_duration) {
            // Keep the existing RF wall-time bound. Remove time for all IDs
            // before starting the worker. If a future duration includes a
            // periodic boundary, removal can suppress that repeat and finish
            // up to one ID early; it can never exceed the requested duration.
            while (id->total_samples > run->samples) {
                uint64_t excess = id->total_samples - run->samples;
                if (id->payload_samples <= excess ||
                    !station_id_init_rate(id, station_store_get(), id->payload_samples - excess,
                                           run->geo.rate, run->fill, run->ctx)) {
                    run->error = ESP_ERR_INVALID_ARG;
                    goto finished;
                }
            }
        }
        run->samples = id->total_samples;
        run->fill = station_id_fill;
        run->ctx = id;
        run->stats->id_samples = id->length;
        run->stats->id_count = (id->total_samples - id->payload_samples) / id->length;
        run->stats->planned_samples = id->total_samples;
    }
    run->done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (worker_task) {
        job_task = task;
        job_arg = run;
        xSemaphoreGive(job_ready);
        xSemaphoreTake(run->done, portMAX_DELAY);
    } else {
        run->error = ESP_ERR_INVALID_STATE; /* radio_s3_init has not run */
    }
finished:
    portENTER_CRITICAL(&state_lock);
    running = false;
    portEXIT_CRITICAL(&state_lock);
    return run->error;
}

static bool known_rate(uint32_t dac_rate)
{
    return dac_rate == RADIO_S3_DAC_RATE || dac_rate == RADIO_S3_DAC_RATE_80M;
}

esp_err_t radio_s3_dry_run_rate(radio_s3_fill_fn fill, void *ctx, uint64_t samples,
                                uint32_t dac_rate, radio_s3_sink_fn sink, void *sink_ctx,
                                radio_s3_dry_stats_t *stats)
{
    if (!fill || !sink || !stats || samples == 0 || !known_rate(dac_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof *stats);
    radio_s3_stats_t timing = {0};
    run_t run = {.fill = fill, .ctx = ctx, .stats = &timing, .samples = samples,
                 .sink = sink, .sink_ctx = sink_ctx, .dry = stats, .geo = geometry(dac_rate)};
    return on_core1(dry_worker, &run);
}

esp_err_t radio_s3_paced_run(uint32_t seconds, uint32_t dac_rate, radio_s3_fill_fn fill, void *ctx,
                             const radio_s3_options_t *options, radio_s3_stats_t *stats)
{
    if (!fill || !stats || seconds == 0 || seconds > 600 || !known_rate(dac_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof *stats);
    stats->min_fill_slack_cycles = UINT32_MAX;
    run_t run = {.seconds = seconds, .samples = (uint64_t)seconds * dac_rate,
                 .identify = station_store_get()[0] != 0, .paced = true,
                 .fill = fill, .ctx = ctx, .stats = stats, .geo = geometry(dac_rate)};
    if (options) run.options = *options;
    esp_err_t error = on_core1(worker, &run);
    if (stats->min_fill_slack_cycles == UINT32_MAX) {
        stats->min_fill_slack_cycles = 0;
    }
    return error;
}

esp_err_t radio_s3_run_bandwidth(uint32_t freq_mhz, uint8_t gain, uint32_t seconds,
                                 uint32_t dac_rate, uint32_t occupied_hz, radio_s3_fill_fn fill,
                                 void *ctx, radio_s3_stats_t *stats)
{
    return radio_s3_run_options(freq_mhz,gain,seconds,dac_rate,occupied_hz,fill,ctx,NULL,stats);
}

esp_err_t radio_s3_run_options(uint32_t freq_mhz, uint8_t gain, uint32_t seconds,
                               uint32_t dac_rate, uint32_t occupied_hz, radio_s3_fill_fn fill,
                               void *ctx, const radio_s3_options_t *options, radio_s3_stats_t *stats)
{
    const int64_t rf = (int64_t)freq_mhz * 1000 + lo_offset_khz;
    if (stats) {
        memset(stats, 0, sizeof *stats);
        stats->requested_rf_khz = rf >= 0 && rf <= UINT32_MAX ? rf : 0;
        stats->lo_offset_khz = lo_offset_khz;
        stats->lo_divided = lo_divided;
        if (rf >= RADIO_S3_FREQ_MIN_MHZ * 1000U && rf <= RADIO_S3_FREQ_MAX_MHZ * 1000U)
            stats->requested_pll_khz = lo_divided ? (6U * rf + 2U) / 5U : rf;
    }
    if (!occupied_hz || occupied_hz > dac_rate || !fill || !stats ||
            freq_mhz < RADIO_S3_FREQ_MIN_MHZ || freq_mhz > RADIO_S3_FREQ_MAX_MHZ ||
            rf < RADIO_S3_FREQ_MIN_MHZ * 1000U || rf > RADIO_S3_FREQ_MAX_MHZ * 1000U ||
            gain > RADIO_S3_GAIN_MAX || seconds == 0 || seconds > RADIO_S3_MAX_SECONDS ||
            !known_rate(dac_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    // Refuse before task creation or any register access.
    if (!station_store_get()[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    stats->min_fill_slack_cycles = UINT32_MAX;
    stats->filter.occupied_hz = occupied_hz;
    stats->filter.requested = radio_filter_get();
    run_t run = {
        .samples = (uint64_t)seconds * dac_rate, .identify = true, .limit_duration = true,
        .freq_mhz = freq_mhz, .gain = gain, .seconds = seconds,
        .fill = fill, .ctx = ctx, .stats = stats,
        .geo = geometry(dac_rate),
    };
    if (options) run.options = *options;
    esp_err_t error = on_core1(worker, &run);
    if (stats->min_fill_slack_cycles == UINT32_MAX) {
        stats->min_fill_slack_cycles = 0;
    }
    return error;
}
