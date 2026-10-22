#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Continuous capture from the S3 radio's dump engine at 16 MS/s into three
 * 64 KiB SRAM banks. The engine writes a 16,384-word ring in whichever bank it
 * owns; ownership moves to the next bank every 12,288 samples (768 us). */
#define CAPTURE_RATE 16000000U
#define CAPTURE_RING 16384U
#define CAPTURE_UNIT 12288U
#define CAPTURE_LATE 2000U      /* a later handoff would overwrite its own unit */

/* Consecutive samples [first, first + count) from a released bank, on core 1.
 * Called once, or twice when the span wraps the ring. Handoffs run in a timer
 * interrupt, so one call may take up to about two units if the average fits in one. */
typedef esp_err_t (*capture_fn)(const uint32_t *words, size_t count, uint64_t first, void *ctx);

typedef struct {
    unsigned mhz, gain, seconds;
    bool promiscuous;   /* Wi-Fi promiscuous receive on channel 1 during the run */
} capture_config_t;

typedef struct {
    uint64_t samples;
    uint32_t units, max_late, max_service, max_clear, max_spin, max_lag;
    uint32_t free_before, free_wifi, free_ready, largest_ready;
    uint8_t gain_max;
    const char *fault;  /* first fault, or NULL */
    esp_err_t error, cleanup;
} capture_stats_t;

esp_err_t capture_run(const capture_config_t *config, capture_fn fn, void *ctx,
                      capture_stats_t *stats);
/* Radio set up as for a run, then read timing: see capture.c bench(). */
esp_err_t capture_bench(const capture_config_t *config, uint32_t results[16],
                        capture_stats_t *stats);
