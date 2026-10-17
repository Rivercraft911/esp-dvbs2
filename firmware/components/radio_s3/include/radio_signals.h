#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// No allocation, flash reads or floating point in fill().
typedef struct {
    uint32_t bandwidth_40m; // occupied Hz at 40 MS/s; doubles at 80 MS/s
    uint32_t tone[40];
} radio_signal_t;

bool radio_signal_init(radio_signal_t *signal, const char *name);
void radio_signal_fill(uint32_t *dst, uint64_t first, size_t count, radio_signal_t *signal);
