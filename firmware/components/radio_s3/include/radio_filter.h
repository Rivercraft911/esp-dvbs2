#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RADIO_FILTER_AUTO, RADIO_FILTER_NARROW, RADIO_FILTER_WIDE, RADIO_FILTER_ANALOG
} radio_filter_mode_t;

/* Raw MMIO words, then BBTOP 0x67 bytes [4,5,6,7,12,13,14,15]. Low six bits
 * are capacitor codes. Readbacks describe configuration, never measured RF bandwidth. */
typedef struct {
    uint32_t analog, digital, channel, cbw, mac;
    uint8_t caps[8];
} radio_filter_regs_t;

typedef struct {
    uint32_t occupied_hz;
    radio_filter_mode_t requested, selected;
    bool applied, matches, retained;
    radio_filter_regs_t before, active, end;
} radio_filter_stats_t;

/* Configuration only. No PHY access until a keyed radio_s3_run call. AUTO selects
 * WIDE above 20 MHz total occupied bandwidth, based on the earlier +/-10 MHz
 * over-the-air observation, not a calibrated passband specification. */
bool radio_filter_set(const char *name);
radio_filter_mode_t radio_filter_get(void);
bool radio_filter_apply(radio_filter_stats_t *stats);
bool radio_filter_finish(radio_filter_stats_t *stats);
/* Append ,"filter":{...} to an open run JSON record, after the radio is off. */
void radio_filter_print(const radio_filter_stats_t *stats);
