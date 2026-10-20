#pragma once
#include "esp_err.h"
#include "samples.h"

typedef struct {
    uint32_t retained_pairs, acquired_pairs, units;
    uint32_t late, overruns, boundary_errors, gaps, sentinel_errors;
    uint32_t max_poll_cycles, max_copy_cycles, max_switch_late_pairs, gain_refreshes;
    uint32_t max_raw_copy_cycles, max_clear_cycles, max_copy_pairs, compact_cycles;
    uint32_t engine_cycles, elapsed_cycles, crc32;
    uint32_t snapshot_pairs, snapshot_crc32;
    struct {
        uint32_t bytes, before_start, after_start, start_cycles;
        uint32_t before_switch, after_switch, switch_cycles;
        uint32_t before_stop, after_stop, control, lanes, bank_start, bank_switch;
        uint32_t edges, first, end;
    } probe;
    uint8_t gain_max;
    bool guards_ok;
    esp_err_t error, cleanup_error;
} rx_report;

#define RX_MEMORY_CASES 7U
typedef struct {
    const char *name;
    uint32_t min_cycles, max_cycles, total_cycles, max_chunk_cycles;
    uint32_t crc32, expected_crc32, errors;
} rx_memory_result;
typedef struct {
    rx_memory_result cases[RX_MEMORY_CASES];
    bool guards_ok;
} rx_memory_report;

esp_err_t rx_capture(uint16_t mhz, uint8_t gain, uint8_t *record, uint32_t *snapshot,
                     rx_report *report, bool probe);
void rx_pattern(uint8_t *record, rx_report *report);
esp_err_t rx_memory(uint8_t *record, rx_memory_report *report);
