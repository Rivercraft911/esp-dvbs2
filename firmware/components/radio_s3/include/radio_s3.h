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
 * allocation, blocking or flash access during a fill. PSRAM reads go through the
 * data cache both cores share and count against the deadline. Return an error to
 * abort transmission. This is a deadline: 16380 words per 356.7 us at 240 MHz.
 */
typedef esp_err_t (*radio_s3_fill_fn)(uint32_t *dst, uint64_t first_sample,
                                    size_t count, void *ctx);

/* Internal heap around Wi-Fi start-up, bytes: free MALLOC_CAP_INTERNAL and the largest free
 * 8-bit internal block. min_free is the heap's low-water mark (since the command started its
 * local monitor, otherwise since boot), read once Wi-Fi is up. */
typedef struct {
    uint32_t free_before, largest_before;   /* before esp_wifi_init */
    uint32_t free_init, largest_init;       /* after esp_wifi_init */
    uint32_t free_up, largest_up;           /* after start, power save off, promiscuous, channel */
    uint32_t min_free;
} radio_s3_wifi_heap_t;

typedef struct {
    uint64_t elapsed_samples; /* Nominal 40 MS/s timeline, not an RF measurement. */
    uint64_t elapsed_cycles;
    uint32_t fills;
    uint32_t switches;
    uint32_t late_fills;      /* Fill completed after its nominal switch time. */
    uint32_t late_switches;   /* Bank write completed after nominal switch time. */
    uint32_t max_fill_cycles;
    uint64_t max_fill_sample;  /* First timeline sample of that slowest bank. */
    uint32_t max_switch_late_cycles;
    uint32_t min_fill_slack_cycles;
    uint32_t fatal_late;      /* Stopped after observing a missed safe-tail deadline. */
    uint32_t fe_base_index;   /* Front-end gain base index found before keying. */
    uint32_t fe_gain_index;   /* Gain slot selected after keying; must be 0 when forced. */
    uint32_t chain;           /* radio_s3_chain_t used for this run. */
    uint32_t id_samples;      /* one CW segment at the nominal DAC rate */
    uint32_t id_count;        /* planned start, periodic and end segments */
    uint64_t planned_samples; /* payload + all IDs */
    radio_s3_wifi_heap_t wifi; /* radio_s3_run and radio_s3_wifi_probe only */
} radio_s3_stats_t;

/* Synchronous, exclusive run on core 1, 240 MHz. Valid requests are integer MHz
 * 2390..2450, seconds 1..60, gain 0..17 (vendor ladder, weakest first). Gain is
 * uncalibrated, not dBm. The component owns Wi-Fi for the session; it must not
 * coexist with another Wi-Fi/BT user. Every return after startup stops the DAC,
 * returns both banks to the CPU, unkeys TX, and stops/deinitializes Wi-Fi.
 * No RF operation happens until this function is called. A persisted callsign is
 * required before any radio access. Start/end and nine-minute CW segments are
 * inserted by a sample-index wrapper INSIDE the requested seconds. Too-short
 * runs fail before worker creation. On a fatal fill/deadline fault, abort immediately: a final ID
 * cannot be guaranteed. No flash write is permitted during a run.
 */
/* How later runs turn the transmit chain on (results/2026-10-16-radio).
 * CW_GAIN (default): start and stop the vendor CW tone, which brings up both DAC
 * branches with the PHY's calibrated settings, then pin gain slot 0 as FORCED does.
 * CW: the CW bring-up alone, backoff (17 - gain) * 2 dB; the vendor power control
 * stays on and the level varies between runs. CW40: CW in the vendor's 40 MHz
 * channel mode. FORCED: front end and gain slot 0 forced directly; on the bench
 * only the I branch reached the mixer and the carrier leak exceeded the signal.
 */
typedef enum {
    RADIO_S3_CHAIN_FORCED, RADIO_S3_CHAIN_CW, RADIO_S3_CHAIN_CW40, RADIO_S3_CHAIN_CW_GAIN
} radio_s3_chain_t;

void radio_s3_set_chain(radio_s3_chain_t mode);

/* Receives the samples the DAC would play, [first, first + count), from a bank that holds
 * sample n at word n % RADIO_S3_BANK_WORDS. */
typedef esp_err_t (*radio_s3_sink_fn)(const uint32_t *bank, uint64_t first, size_t count,
                                      void *ctx);

typedef struct {
    uint32_t fills;
    uint32_t max_fill_cycles;
    uint32_t overlap_words;      /* lead and tail words compared at bank switches */
    uint32_t overlap_mismatches; /* must be 0: the fill must depend on sample index only */
} radio_s3_dry_stats_t;

/* The same fill sequence as radio_s3_run, into the same bank memory, on core 1, without the
 * radio: bank j holds [j * STEP - LEAD, ... + BANK_WORDS) and plays [j * STEP, (j + 1) * STEP).
 * At every switch both banks' shared lead and tail words are compared, then the played samples
 * go to sink. No RF. Not timed against the DAC: the sink may be slow. */
esp_err_t radio_s3_dry_run(radio_s3_fill_fn fill, void *ctx, uint64_t samples,
                           radio_s3_sink_fn sink, void *sink_ctx, radio_s3_dry_stats_t *stats);

/* Radio off: radio_s3_run's bank schedule on core 1 for 1..600 seconds, the same fills into
 * the same bank memory against the same cycle-counter deadlines, with no Wi-Fi, PHY, DAC or
 * bank-ownership register access. Nothing is transmitted, so the DAC's own reads of the bank
 * memory are absent. Stats as for radio_s3_run; the fe_* fields and chain stay 0.
 * With a configured callsign, inserts the same start/periodic/end IDs as RF;
 * otherwise keeps the original unwrapped run. seconds counts payload time. */
esp_err_t radio_s3_paced_run(uint32_t seconds, radio_s3_fill_fn fill, void *ctx,
                             radio_s3_stats_t *stats);

/* One of the two 64 KiB DAC bank regions (n = 0 or 1), reserved at startup. Between
 * runs the CPU may use them as plain internal RAM; a run overwrites both. */
uint32_t *radio_s3_bank(unsigned n);

esp_err_t radio_s3_run(uint32_t freq_mhz, uint8_t gain, uint32_t seconds,
                       radio_s3_fill_fn fill, void *ctx, radio_s3_stats_t *stats);

/* Wi-Fi memory probe: radio_s3_run's set-up on core 1 (the 30 s run's station ID, the worker
 * task and stack) and its Wi-Fi start-up (init, NULL mode, start with the PHY's calibration,
 * power save off, promiscuous receive, channel 1), then the same stop and deinit. It stops
 * before radio_s3_run's first register access: no retune, keying, gain, tone, bank grant or
 * DAC. NULL mode sends no frames. Fills stats->wifi; needs a stored callsign. */
esp_err_t radio_s3_wifi_probe(radio_s3_stats_t *stats);
