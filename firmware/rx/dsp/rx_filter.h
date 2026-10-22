#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RX_FILTER_TAPS = 32, RX_FILTER_PHASES = 64,
       RX_FILTER_INPUT = 512, RX_FILTER_OUTPUT = 64, RX_FILTER_PLANE = 576 };
#define RX_FILTER_STEP_MIN UINT64_C(16535624090) /* ceil(3.85 * 2^32) */
#define RX_FILTER_STEP_MAX UINT64_C(17824114278) /* floor(4.15 * 2^32) */

/* Position is the START of a 32-sample window. Its nominal center is
 * origin + fraction/2^32 + 15.5 input samples. Nearest-bank selection may
 * carry into origin; advancing the NCO never discards its fractional bits.
 * Pulse taps are signed Q7, derivative taps signed Q15 per input sample.
 * Both outputs are signed Q6: floor((sum + 2^(shift-1))/2^shift), saturated
 * to S16; shifts are 1 and 9. The bank is experimental, not adopted RX. */
typedef struct {
    _Alignas(16) int8_t plane[2][RX_FILTER_PLANE];
    uint64_t received, base, origin, step;
    uint32_t fraction;
    uint16_t count, bias;
    bool failed;
} rx_filter;

typedef struct { size_t consumed, written; } rx_filter_progress;
typedef struct { uint32_t cases, errors; bool native; } rx_filter_check;

/* State must be 16-byte aligned and internal RAM on S3. Buffers must not
 * overlap state, each other, or progress. No allocation; single owner.
 * init permits an arbitrary absolute origin in the incoming stream. */
bool rx_filter_init(rx_filter *s, uint64_t origin, uint32_t fraction, uint64_t step);
bool rx_filter_set_step(rx_filter *s, uint64_t step);

/* At most one <=512-pair input chunk is accepted per call. Pending outputs
 * are drained first; a full output can therefore return consumed == 0.
 * Call again with the unconsumed suffix, including pairs == 0 to drain.
 * capacity is 1..64; derivative_mask bit j requests derivative output j.
 * Unrequested derivative slots are zero. Output buffers may be external.
 * On a runtime failure, progress retains the completed prefix; reset the
 * failed state. Invalid arguments perform no writes. This is not a flush:
 * no implicit zero padding or extrapolated end-of-stream outputs. */
bool rx_filter_push(rx_filter *s, const int8_t *iq, size_t pairs,
                    int16_t (*pulse)[2], int16_t (*derivative)[2],
                    size_t capacity, uint64_t derivative_mask,
                    rx_filter_progress *progress);

int16_t rx_filter_round(int64_t sum, unsigned shift);
void rx_filter_preflight(rx_filter_check *result);

extern const int8_t rx_filter_pulse[RX_FILTER_PHASES][RX_FILTER_TAPS];
extern const int16_t rx_filter_derivative[RX_FILTER_PHASES][RX_FILTER_TAPS];

/* Private leaf contract, also exposed for radio-off primitive controls:
 * coefficient and sums aligned16/4; i/q have equal byte alignment, and
 * each aligned-down pointer has 32 valid bytes when aligned, or 48 valid
 * bytes when unaligned. The logical dot always uses exactly32 samples.
 * Coefficients are internal. QR0..5 caller-clobbered; PS, CPENABLE, ACCX
 * and SAR_BYTE restored and checked. No scalar SAR/QACC/FCR changes.
 * Caller must not depend on QR across the call. False means no admission. */
bool rx_filter_dot_s8(const int8_t *i, const int8_t *q,
                      const int8_t *coefficient, int32_t sums[2]);
