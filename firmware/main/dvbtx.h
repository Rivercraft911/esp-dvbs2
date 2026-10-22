#pragma once

#include <stdint.h>
#include <stdbool.h>

// The sps argument is an integer except DVB_SPS_12_5, meaning exactly 12/5.
#define DVB_SPS_12_5 12u

// Configure frame-boundary VCM for the next run: off, on (same modulation), mixed (5m, automatic axes), or hold + MODCOD pattern.
void dvbtx_vcm_command(const char *args);

// Select short (16,200 bit) FECFRAMEs for the next dvb, dvbdump or dvbpace call; QPSK MODCOD 4..10 at 1 Msym/s.
void dvbtx_set_short(bool enabled);

// QPSK/8PSK encoded on core 0 and shaped on core 1. Measured 80 MS/s modes
// use QPSK blocks at 3/4 sps and short 8PSK at 4 sps. QPSK 12/5 sps uses a persistent prepare/render queue, roll-off .20.
// Timing evidence and remaining margins are in docs/measurements.md.
// fixture repeats dvbpace's prepared frame; requires a queued rate and payload input off.
void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                unsigned sps, uint32_t dac_rate, bool fixture);

// dvbdump command: the same encoder and fill sequence without the radio. Every decim-th played
// sample goes out over USB as int16 I/Q after a record carrying binary_bytes. dac_rate picks the
// bank geometry and the measured kernel (radio_s3.h).
void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                unsigned stall_ms, unsigned sps, uint32_t dac_rate);

// dvbpace command: the same encoder, fills and bank deadlines as dvb for `seconds`, timed against
// the cycle counter, at 40 or 80 MS/s. Queued and VCM modes keep Wi-Fi NULL/RX up
// (radio_s3_paced_wifi_rate); other fixed modes leave Wi-Fi off. No DAC or TX keying.
void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                 unsigned sps, uint32_t dac_rate);

// wifimem command: radio_s3_wifi_probe (Wi-Fi start-up and shutdown, no keying), with the
// transmitter's memory for modcod/sps allocated as before a dvb run, or nothing for modcod 0.
// Reports internal heap before and after each step and the low-water mark.
// The probe never plays samples.
void dvbtx_wifi(unsigned modcod, unsigned sps, uint32_t dac_rate);

// Timed experimental options. fused tries the larger table only with room for
// all 54 pages and the existing floor/guard; otherwise keeps scaled rendering.
void dvbtx_run_options(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level,
                        unsigned sps, uint32_t dac_rate, bool fixture, bool phy_solo, bool fused);
void dvbtx_pace_options(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall_ms,
                         unsigned sps, uint32_t dac_rate, bool phy_solo, bool fused);
