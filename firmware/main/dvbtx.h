#pragma once

#include <stdint.h>
#include <stdbool.h>

// QPSK (sps=40) or 8PSK (sps=8/4), encoded on core 0 and shaped on core 1, at 40 or (8PSK)
// 80 MS/s.
void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps, uint32_t dac_rate);

// dvbdump command: the same encoder and fill sequence without the radio. Every decim-th played
// sample goes out over USB as int16 I/Q after a record carrying binary_bytes. dac_rate picks the
// bank geometry (radio_s3.h); 80 MS/s takes 8PSK (4 or 8 sps) only.
// short_pulse selects the six-symbol experiment, only at 4 sps and 80 MS/s.
void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                unsigned stall_ms, unsigned sps, uint32_t dac_rate, bool short_pulse);

// dvbpace command: the same encoder, fills and bank deadlines as dvb for `seconds`, timed against
// the cycle counter with the radio off (radio_s3_paced_run_rate), at 40 or 80 MS/s.
void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps, uint32_t dac_rate, bool short_pulse);

// wifimem command: radio_s3_wifi_probe (Wi-Fi start-up and shutdown, no keying), with the
// transmitter's memory for modcod/sps allocated as before a dvb run, or nothing for modcod 0.
// Reports internal heap before and after each step and the low-water mark.
void dvbtx_wifi(unsigned modcod, unsigned sps);
