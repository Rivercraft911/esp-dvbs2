#pragma once
#include "samples.h"

#define RX_PACK_MAX_PAIRS (RX_UNIT_PAIRS + RX_LATE_PAIRS)
#define RX_PACK_STAGE_BYTES (2U * RX_PACK_MAX_PAIRS + 192U)

/* CPU-owned bank, disjoint private output with (out & 15)==(2*first & 15).
 * Reject every sentinel, including across wrap, before changing any output.
 * PIE callers own q0..q7 and SAR. No bank clearing occurs in this function. */
bool rx_pack_bank(const uint32_t *bank, uint16_t first, size_t pairs, uint8_t *out);
void rx_pack_clear(uint32_t *bank);

typedef struct { size_t head, bulk, tail; } rx_packed_plan;
bool rx_pack_copy_plan(uintptr_t source, uintptr_t out, size_t bytes, rx_packed_plan *plan);
