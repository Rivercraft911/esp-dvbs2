#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// No allocation, flash reads or floating point in fill(). The tx diagnostic
// borrows the existing shaper scratch store, including the wide chirp.
#define RADIO_SIGNAL_MAX_WORDS 4001U
typedef struct {
    uint32_t bandwidth_40m; // occupied Hz at 40 MS/s; doubles at 80 MS/s
    uint32_t length;
    uint32_t tone[RADIO_SIGNAL_MAX_WORDS];
} radio_signal_t;

bool radio_signal_init(radio_signal_t *signal, const char *name);
void radio_signal_fill(uint32_t *dst, uint64_t first, size_t count, radio_signal_t *signal);
