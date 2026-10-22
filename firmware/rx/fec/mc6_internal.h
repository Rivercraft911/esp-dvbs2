#pragma once

#include "rx_mc6.h"

/* Private leaf ABI. These offsets are also used by mc6_pie.S. */
typedef struct {
    uint32_t before[4];
    int16_t q[10][8];
    uint16_t magnitudes[8];
    uint16_t metadata[8];
    uint16_t high[8];
    uint32_t after[4];
} rx_mc6_tile;

_Static_assert(sizeof(rx_mc6_check) == 4, "four bytes per check");
_Static_assert(sizeof(rx_mc6_tile) == 240, "bounded internal tile");
_Static_assert(offsetof(rx_mc6_tile, q) == 16, "leaf q offset");
_Static_assert(offsetof(rx_mc6_tile, magnitudes) == 176, "leaf magnitude offset");
_Static_assert(offsetof(rx_mc6_tile, metadata) == 192, "leaf metadata offset");
_Static_assert(offsetof(rx_mc6_tile, high) == 208, "leaf high offset");

void rx_mc6_leaf(rx_mc6_tile *tile);
