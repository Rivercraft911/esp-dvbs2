#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RX_CARRIER_SYMBOLS = 33282, RX_CARRIER_BITS = 64800,
       RX_CARRIER_WORD_BYTES = 8100, RX_CARRIER_GOLD_BYTES = 8298,
       RX_CARRIER_KNOWN = 882, RX_CARRIER_ANCHORS = 23 };

typedef struct {
    uint64_t first_native_sample, input_origin, input_step_q32;
    uint32_t input_fraction;
    float symbol_rate_hz;
    unsigned modcod; /* Normal QPSK, pilots on. Currently MC4 or MC6 only. */
} rx_carrier_metadata;

typedef struct {
    rx_carrier_metadata metadata;
    float slope, amplitude, phase[23];
    float timing_gradient[23], timing_coherence[23];
    uint32_t timing_observed, base[22], step[22];
    uint32_t cursor, emitted;
    int16_t high, negative_high, low;
    uint16_t header_errors, pilot_errors;
    bool prepared, failed;
} rx_carrier;

/* Generate the fixed PL Gold code once, outside frame timing. The caller owns
 * the8298B cache and keeps it immutable during estimate/project. No payload. */
void rx_carrier_gold(uint8_t out[RX_CARRIER_GOLD_BYTES]);
bool rx_carrier_header(unsigned modcod, int8_t signs[90][2]);

/* Q6 frame is immutable until projection completes. Optional sparse Q6
 * derivatives are header90 then22 pilot blocks of36, per raw input sample.
 * Timing fields are observations, NOT a qualified closed-loop controller.
 * Retains the23-pilot/header magnitude median and future-pilot lookahead.
 * Failed admission leaves state unchanged. Buffers/state must be disjoint. */
bool rx_carrier_estimate(rx_carrier *state, const int16_t frame[RX_CARRIER_SYMBOLS][2],
                        const int16_t derivative[RX_CARRIER_KNOWN][2],
                        const uint8_t gold[RX_CARRIER_GOLD_BYTES],
                        const rx_carrier_metadata *metadata);

/* Bounded projection of1..64 PL symbols. Emits only the32400 data symbols,
 * preserving bit order I then Q, MSB-first packed word. Output buffers are
 * full-frame storage; completed prefixes remain valid between calls. Init
 * cursor is0; completion is cursor==33282 && emitted==64800. No allocation.
 * New numeric contract: nearest4096-phase Q15 phasor, symmetric +/-32767;
 * carrier+Gold phase fused BEFORE projection. Product floor>>16 gives Q5;
 * four levels use exact integer comparisons to amplitude-derived cuts.
 * This is not bit-exact equivalence to the old float receiver. */
bool rx_carrier_project(rx_carrier *state, const int16_t frame[RX_CARRIER_SYMBOLS][2],
                       const uint8_t gold[RX_CARRIER_GOLD_BYTES], unsigned symbols,
                       int8_t llr[RX_CARRIER_BITS], uint8_t word[RX_CARRIER_WORD_BYTES]);

/* Independently testable integer primitive. Scalar, no PIE context ownership.
 * Sum bound2*32768*32767<INT32_MAX; floor division by65536 fits signed16. */
void rx_carrier_phasor(uint32_t phase, int16_t out[2]);
void rx_carrier_rotate(const int16_t sample[2], uint32_t phase, int16_t out[2]);
