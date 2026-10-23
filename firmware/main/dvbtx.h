#pragma once

#include <stdint.h>
#include <stdbool.h>

// The sps argument is an integer except DVB_SPS_12_5, meaning exactly 12/5.
#define DVB_SPS_12_5 12u

// Configure frame-boundary VCM for the next run: off, on (same modulation), mixed (5m, automatic axes), or hold + MODCOD pattern.
void dvbtx_vcm_command(const char *args);

// Select short (16,200 bit) FECFRAMEs for the next dvb, dvbdump or dvbpace call:
// QPSK MODCOD 4..10 at 1 or 5 Msym/s, 8PSK MODCOD 12..16 at 5 Msym/s.
void dvbtx_set_short(bool enabled);

// dvb command: QPSK/8PSK encoded on core 0 and shaped on core 1 while keyed. 80 MS/s modes use
// QPSK blocks at 3/4 sps, short-pulse 8PSK at 4 sps, the 8PSK 26m667 page queue at 3 sps and
// the QPSK 12/5 sps axis queue (roll-off .20). fixture repeats dvbpace's prepared frame; it
// requires a queued rate and payload input off. phy_solo deinitializes Wi-Fi before playback.
// Timing evidence and remaining margins are in docs/measurements.md.
void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
               unsigned sps, uint32_t dac_rate, bool fixture, bool phy_solo);

// dvbdump command: the same encoder and fill sequence without the radio. Every decim-th played
// sample goes out over USB as int16 I/Q after a record carrying binary_bytes. dac_rate picks the
// bank geometry and the measured kernel (radio_s3.h).
void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                unsigned stall_ms, unsigned sps, uint32_t dac_rate);

// dvbpace command: the same encoder, fills and bank deadlines as dvb for `seconds`, timed against
// the cycle counter, at 40 or 80 MS/s. Queued, VCM and PHY-solo paces start Wi-Fi as a keyed run
// does (radio_s3_paced_wifi_options); other fixed modes leave Wi-Fi off. No DAC or TX keying.
void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                unsigned sps, uint32_t dac_rate, bool phy_solo);

// wifimem command: radio_s3_wifi_probe (Wi-Fi start-up and shutdown, no keying), with the
// transmitter's memory for modcod/sps allocated as before a dvb run, or nothing for modcod 0.
// Reports internal heap before and after each step and the low-water mark.
// The probe never plays samples.
void dvbtx_wifi(unsigned modcod, unsigned sps, uint32_t dac_rate);
