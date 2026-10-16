#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RADIO_S3_DAC_RATE 40000000U
#define RADIO_S3_BANK_WORDS 16380U
#define RADIO_S3_LEAD_WORDS 64U
#define RADIO_S3_TAIL_WORDS 2048U
#define RADIO_S3_STEP_WORDS (RADIO_S3_BANK_WORDS - RADIO_S3_LEAD_WORDS - RADIO_S3_TAIL_WORDS)
#define RADIO_S3_GAIN_DEFAULT 0U
#define RADIO_S3_GAIN_MAX 17U

/* Fill consecutive timeline samples, packed signed I[9:0], Q[19:10]. dst needs
 * aligned 32-bit stores. Calls split at a bank wrap, so sample n always lives
 * at physical word n % RADIO_S3_BANK_WORDS. For handoff k the entire bank is
 * populated with [k*STEP-LEAD, k*STEP-LEAD+BANK_WORDS), except the first bank
 * starts at sample zero. Matching lead/tail samples make early/late selection
 * harmless inside that overlap, assuming the documented free-running DAC.
 *
 * The callback and its lookup tables must be in internal RAM/IRAM; no logging,
 * allocation, blocking or flash/PSRAM access during a fill. Return an error to
 * abort transmission. This is a deadline: 16380 words per 356.7 us at 240 MHz.
 */
typedef esp_err_t (*radio_s3_fill_fn)(uint32_t *dst, uint64_t first_sample,
                                    size_t count, void *ctx);

typedef struct {
    uint64_t elapsed_samples; /* Nominal 40 MS/s timeline, not an RF measurement. */
    uint64_t elapsed_cycles;
    uint32_t fills;
    uint32_t switches;
    uint32_t late_fills;      /* Fill completed after its nominal switch time. */
    uint32_t late_switches;   /* Bank write completed after nominal switch time. */
    uint32_t max_fill_cycles;
    uint32_t max_switch_late_cycles;
    uint32_t min_fill_slack_cycles;
    uint32_t fatal_late;      /* Stopped after observing a missed safe-tail deadline. */
    uint32_t fe_base_index;   /* Front-end gain base index found before keying. */
    uint32_t fe_gain_index;   /* Gain slot selected after keying; must be 0. */
} radio_s3_stats_t;

/* Synchronous, exclusive run on core 1, 240 MHz. Valid requests are integer MHz
 * 2390..2450, seconds 1..60, gain 0..17 (vendor ladder, weakest first). Gain is
 * uncalibrated, not dBm. The component owns Wi-Fi for the session; it must not
 * coexist with another Wi-Fi/BT user. Every return after startup stops the DAC,
 * returns both banks to the CPU, unkeys TX, and stops/deinitializes Wi-Fi.
 * No RF operation happens until this function is called.
 */
esp_err_t radio_s3_run(uint32_t freq_mhz, uint8_t gain, uint32_t seconds,
                       radio_s3_fill_fn fill, void *ctx, radio_s3_stats_t *stats);
