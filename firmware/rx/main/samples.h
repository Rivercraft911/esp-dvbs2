#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RX_BANK_PAIRS 16384U
#define RX_BANK_MASK (RX_BANK_PAIRS - 1U)
#define RX_SENTINEL UINT32_C(0xa5a5a5a5)
#define RX_EDGE_GUARD 64U
#ifndef RX_RECORD_PAIRS
#define RX_RECORD_PAIRS 1600000U
#endif
#if RX_RECORD_PAIRS < 1600000 || RX_RECORD_PAIRS > 2000000
#error "RX_RECORD_PAIRS must be 1600000..2000000"
#endif
#define RX_RECORD_BYTES (2U * RX_RECORD_PAIRS)
#define RX_CAPTURE_BYTES (4U * RX_RECORD_PAIRS)

typedef struct { uint16_t before, after; } rx_edge;

/* A switch's actual index is recovered from sentinel transitions on both banks.
 * Brackets wider than 64 pairs or ambiguous transitions are rejected. */
bool rx_find_edge(const uint32_t *bank, rx_edge bracket, bool entering,
                  uint16_t *index);
bool rx_span(const uint32_t *bank, rx_edge begin, rx_edge end,
             uint16_t *first, size_t *pairs);
/* Caller owns this bank. Preserve raw words while the engine writes another
 * bank; validate sentinels and compact only after stopping the engine. */
void rx_copy(const uint32_t *bank, uint16_t first, size_t pairs, uint32_t *out);
bool rx_compact(uint32_t *record, size_t pairs);

typedef struct { size_t head, bulk, tail, split; } rx_copy_plan;
bool rx_plan_copy(uint16_t first, size_t pairs, rx_copy_plan *plan);
