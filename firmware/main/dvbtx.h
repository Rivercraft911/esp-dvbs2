#pragma once

#include <stdint.h>
#include <stdbool.h>

enum { DVB_PULSE_LEGACY, DVB_PULSE_SHORT, DVB_PULSE_QUADS };

// QPSK/8PSK encoded on core 0 and shaped on core 1. QPSK adds 3 sps at 80 MS/s;
// DVB_PULSE_QUADS selects its four-symbol kernel at 3 or 4 sps. Legacy rates keep their CRCs.
void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps, uint32_t dac_rate, unsigned pulse);

// dvbdump command: the same encoder and fill sequence without the radio. Every decim-th played
// sample goes out over USB as int16 I/Q after a record carrying binary_bytes. dac_rate picks the
// bank geometry (radio_s3.h). DVB_PULSE_SHORT selects the six-symbol 8PSK
// experiment, only at 4 sps and 80 MS/s.
void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                unsigned stall_ms, unsigned sps, uint32_t dac_rate, unsigned pulse);

// dvbpace command: the same encoder, fills and bank deadlines as dvb for `seconds`, timed against
// the cycle counter with the radio off (radio_s3_paced_run_rate), at 40 or 80 MS/s.
void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps, uint32_t dac_rate, unsigned pulse);

// wifimem command: radio_s3_wifi_probe (Wi-Fi start-up and shutdown, no keying), with the
// transmitter's memory for modcod/sps allocated as before a dvb run, or nothing for modcod 0.
// Reports internal heap before and after each step and the low-water mark.
// The pulse selection allocates the matching layout; the probe never plays samples.
void dvbtx_wifi(unsigned modcod, unsigned sps, uint32_t dac_rate, unsigned pulse);
