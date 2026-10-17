#pragma once

#include <stdint.h>

// QPSK (sps=40) or 8PSK (sps=8/4), encoded on core 0 and shaped on core 1.
void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps);

// dvbdump command: the same encoder and fill sequence without the radio. Every decim-th played
// sample goes out over USB as int16 I/Q after a record carrying binary_bytes.
void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                unsigned stall_ms, unsigned sps);

// dvbpace command: the same encoder, fills and bank deadlines as dvb for `seconds`, timed against
// the cycle counter with the radio off (radio_s3_paced_run).
void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps);

// wifimem command: radio_s3_wifi_probe (Wi-Fi start-up and shutdown, no keying), with the
// transmitter's memory for modcod/sps allocated as before a dvb run, or nothing for modcod 0.
// Reports internal heap before and after each step and the low-water mark.
void dvbtx_wifi(unsigned modcod, unsigned sps);
