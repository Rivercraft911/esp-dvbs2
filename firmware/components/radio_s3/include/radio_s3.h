#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RADIO_S3_DAC_RATE 40000000U      /* DAC_CTRL bit 15 clear; the keyed rate */
#define RADIO_S3_DAC_RATE_80M 80000000U  /* bit 15 set; radio-off runs only so far */
#define RADIO_S3_BANK_WORDS 16380U

/* Bank geometry (docs/bank-geometry.md). Bank k plays samples [k*STEP, (k+1)*STEP) and also
 * holds LEAD samples before and TAIL after them, so a switch up to LEAD samples early or TAIL
 * late plays the same samples. LEAD + STEP + TAIL = BANK_WORDS, so every fill rewrites the
 * whole bank and is due every STEP samples. The switch is aimed LEAD/2 early. All multiples
 * of 4, which keeps whole-symbol bank starts 16-byte aligned.
 * 40 MS/s, keyed on air: esp-sdr-zephyr's 64 and 2048; a bank is due every 85,608 cycles.
 * 80 MS/s: LEAD 128 keeps the 40 MS/s lead in time (0.8 us each side of the aim; the DAC's
 * start latency is unmeasured). TAIL 512 is 7.2 us of late tolerance, above the latest
 * switch ever recorded (1,339 cycles, plus 96 for the shorter aim at 80 MS/s) with the
 * switch wait masked or not. STEP 15,740: 47,220 cycles per bank instead of 42,804. */
#define RADIO_S3_LEAD_WORDS 64U
#define RADIO_S3_TAIL_WORDS 2048U
#define RADIO_S3_STEP_WORDS (RADIO_S3_BANK_WORDS - RADIO_S3_LEAD_WORDS - RADIO_S3_TAIL_WORDS)
#define RADIO_S3_LEAD_WORDS_80M 128U
#define RADIO_S3_TAIL_WORDS_80M 512U
#define RADIO_S3_STEP_WORDS_80M (RADIO_S3_BANK_WORDS - RADIO_S3_LEAD_WORDS_80M - \
                                 RADIO_S3_TAIL_WORDS_80M)
#define RADIO_S3_STEP_FOR(rate) \
    ((rate) == RADIO_S3_DAC_RATE_80M ? RADIO_S3_STEP_WORDS_80M : RADIO_S3_STEP_WORDS)
#define RADIO_S3_LEAD_FOR(rate) \
    ((rate) == RADIO_S3_DAC_RATE_80M ? RADIO_S3_LEAD_WORDS_80M : RADIO_S3_LEAD_WORDS)
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
 * abort transmission. This is a deadline: 16380 words per STEP samples, 356.7 us at 40 MS/s
 * and 196.75 us at 80 MS/s (240 MHz CPU).
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
    uint64_t elapsed_samples; /* Nominal timeline at the run's rate, not an RF measurement. */
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

/* Switch probe for later radio_s3_run calls (`tx ... gap`), never for DVB. After each fill,
 * silence the bank's own lead and tail, where both banks normally agree. Every switch then
 * plays |switch sample - k*STEP| silent samples, the bank's aim plus the DAC's lag, which an
 * RF capture measures (host/capture.py --signal gap). */
void radio_s3_set_gaps(bool on);

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
/* The same at dac_rate's bank geometry (RADIO_S3_DAC_RATE or RADIO_S3_DAC_RATE_80M). */
esp_err_t radio_s3_dry_run_rate(radio_s3_fill_fn fill, void *ctx, uint64_t samples,
                                uint32_t dac_rate, radio_s3_sink_fn sink, void *sink_ctx,
                                radio_s3_dry_stats_t *stats);

/* Radio off: radio_s3_run's bank schedule on core 1 for 1..600 seconds, the same fills into
 * the same bank memory against the same cycle-counter deadlines, with no Wi-Fi, PHY, DAC or
 * bank-ownership register access. Nothing is transmitted, so the DAC's own reads of the bank
 * memory are absent. Stats as for radio_s3_run; the fe_* fields and chain stay 0.
 * With a configured callsign, inserts the same start/periodic/end IDs as RF;
 * otherwise keeps the original unwrapped run. seconds counts payload time. */
esp_err_t radio_s3_paced_run(uint32_t seconds, radio_s3_fill_fn fill, void *ctx,
                             radio_s3_stats_t *stats);
/* The same at dac_rate: its geometry and 240e6 / dac_rate cycles per sample. At 80 MS/s the
 * IDs are the 40 MS/s ID's samples (half its air time); only their bank timing is real. */
esp_err_t radio_s3_paced_run_rate(uint32_t seconds, uint32_t dac_rate, radio_s3_fill_fn fill,
                                  void *ctx, radio_s3_stats_t *stats);

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
