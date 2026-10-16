#pragma once

#include <stdint.h>

// dvb command: encode QPSK frames on the chip and transmit them at 1 Msym/s (dvbtx.c).
void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain, unsigned level);

// dvbdump command: the same encode and fill sequence without the radio. Every decim-th played
// sample goes out over USB as int16 I/Q after a record carrying binary_bytes.
void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim);
