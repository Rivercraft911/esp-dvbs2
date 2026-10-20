#pragma once
#include "esp_err.h"
#include "samples.h"

typedef struct {
    uint32_t retained_pairs, acquired_pairs, units;
    uint32_t late, overruns, boundary_errors, gaps, sentinel_errors;
    uint32_t max_poll_cycles, max_copy_cycles, max_switch_late_pairs, gain_refreshes;
    uint32_t engine_cycles, elapsed_cycles, crc32;
    uint32_t snapshot_pairs, snapshot_crc32;
    uint8_t gain_max;
    bool guards_ok;
    esp_err_t error, cleanup_error;
} rx_report;

esp_err_t rx_capture(uint16_t mhz, uint8_t gain, uint8_t *record, uint32_t *snapshot,
                     rx_report *report);
void rx_pattern(uint8_t *record, rx_report *report);
