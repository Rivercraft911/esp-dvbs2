#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Test data stays in internal SRAM. No flash reads, allocation or floating point in fill().
// Tag: seed 1, output state bit 0 as BPSK (+256 for 1), then right-shift Galois step
// s = (s >> 1) ^ ((0 - (s & 1)) & 0x80200003). Period 2^32-1 DAC samples.
typedef struct {
    bool tag;
    uint32_t tone[40];
    uint32_t rows[256][8];
    uint32_t advance[256];
    uint32_t jump[32][32];
} radio_signal_t;

bool radio_signal_init(radio_signal_t *signal, const char *name);
void radio_signal_fill(uint32_t *dst, uint64_t first, size_t count, radio_signal_t *signal);
