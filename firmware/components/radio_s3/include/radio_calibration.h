#ifndef RADIO_CALIBRATION_H
#define RADIO_CALIBRATION_H

#include "esp_err.h"
#include "radio_filter.h"

/* Optional profile measured only on XIAO 44:b1:76:b0:e5:8c at 2420 MHz,
 * gain 0 and 40 MS/s. Commands run between synchronous radio sessions. */
typedef struct {
    bool requested, guarded, applied, restored;
    uint32_t iq_saved, iq_active, iq_restored;
    uint32_t dc_saved[2], dc_active[2], dc_restored[2];
} radio_calibration_stats_t;
void radio_calibration_command(const char *args);
/* Clear per-run statistics before checking the profile. */
void radio_calibration_attempt(radio_calibration_stats_t *stats, bool keyed);
esp_err_t radio_calibration_begin(uint32_t mhz, unsigned gain, uint32_t dac_rate,
                                  radio_calibration_stats_t *stats);
bool radio_calibration_filter(radio_filter_stats_t *stats);
bool radio_calibration_restore(void);
void radio_calibration_end(radio_calibration_stats_t *stats);
void radio_calibration_print(const radio_calibration_stats_t *stats);

#endif
