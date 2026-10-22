#ifndef DVBS2RX_KERNELS_H
#define DVBS2RX_KERNELS_H

#include <stddef.h>
#include <stdint.h>

/* Front-end kernels. kernels.c is the reference; kernels_pie.S gives identical results on
 * the S3. Buffers are 16-byte aligned.
 *
 * rx_unpack: 16 dump words per block become 16 I and 16 Q samples, floor(s10 / 4). */
#define RX_UNPACK_BLOCK 16u
void rx_unpack(const uint32_t *words, size_t blocks, int8_t *i, int8_t *q);

/* Matched filter at chosen times. For each step k, with x = i (out[2k]) or q (out[2k + 1]):
 *   out[2k + x] = sum_{j < 16 * vectors} x[base_k + j] * coef_k[j]
 * base_k is a multiple of 16. coef_k is a 16-byte-aligned set of 16 * vectors int8 taps: the
 * pulse shifted by the step's offset within the first vector and by its fractional phase,
 * zero-padded. x must stay readable for 16 bytes past each window. vectors is 2..16. */
typedef struct {
    uint32_t base;
    const int8_t *coef;
} rx_mf_step_t;
void rx_mf(const int8_t *i, const int8_t *q, const rx_mf_step_t *steps, size_t n,
           unsigned vectors, int32_t *out);

/* Portable references, also built on the S3 for comparison. */
void rx_unpack_c(const uint32_t *words, size_t blocks, int8_t *i, int8_t *q);
void rx_mf_c(const int8_t *i, const int8_t *q, const rx_mf_step_t *steps, size_t n,
             unsigned vectors, int32_t *out);

#endif
