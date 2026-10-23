#ifndef DVBS2RX_DEMOD_PIE_H
#define DVBS2RX_DEMOD_PIE_H

#include <stdint.h>

/* The demodulator's heavy kernels. The portable versions in demod.c define the results;
 * demod_pie.S gives the same bytes on the S3. Arithmetic is that of the PIE lanes:
 * mul(a, b) = the low 16 bits of floor(a b / 2^14), and sums saturate to int16.
 *
 * Data symbols sit in planar int16 rings (I and Q), 16-byte aligned, 8 symbols a group. */
typedef struct {
    int16_t co[8], so[8];   /* cos and sin of k inc for lane k, Q14 */
    int16_t lr[8], li[8];   /* carrier leak after derotation, every lane */
    int16_t dr[8], di[8];   /* receiver DC, every lane */
    int16_t sel[2][8];      /* lane k: 1 << 2k and 1 << (2k + 1), the scrambling code bits */
    int16_t gain[8];        /* soft-bit scale, Q14, every lane */
    int16_t gain0[8];       /* 8PSK first bit: gain / sqrt(2) */
    int16_t hi[8], lo[8];   /* 127, -127 */
    int16_t weak[8];        /* 16 */
    int16_t weight[2][8];   /* lane k: bit 15 - k and 7 - k */
    int16_t one[8];
    const int16_t *table;   /* sin(2 pi k / 1024), Q14 */
} __attribute__((aligned(16))) demod_run_t;  /* the PIE code relies on this layout */

/* Matched filter at the tracked times, into planar int16 outputs.
 * 1 Msym/s (demod_mf): output k at time t + k step (Q24 dump samples, with 2^21 added to round
 * to a quarter sample): the 112-byte coefficient set for its offset and phase against both
 * rings from the 16-aligned base (demod_tables.h), each sum >> 4, added to sum. t advances.
 * 5 Msym/s (demod_mf5): output k at t + (k mod 5) step + 16 (k div 5) samples (five steps
 * taken as 16 samples within a call), t with 2^18 added to round to 1/32 sample. Centre c,
 * window base b = 16 floor((floor(c) - 8) / 16): the 32 samples from b against rx_mf5 row
 * (c mod 1) * 32 from byte 24 - (floor(c) - b), each sum >> RX_MF5_SHIFT. The PIE version
 * also writes up to 4 outputs either side of [0, n). Neither touches t or sum. */
#define DEMOD_RING 4096u        /* sample ring per axis */
#define DEMOD_STRIDE 4352u      /* rq - ri: the ring and its mirrored head, 17 x 256 */
typedef struct {
    const int8_t *ri, *rq;      /* sample rings, 16-byte aligned */
    const int8_t *sets;         /* rx_mf_sets or rx_mf5 */
    uint32_t t_lo, t_hi;
    uint32_t step;
    int32_t sum_i, sum_q;
} demod_mf_t;                   /* the PIE code relies on this layout */

void demod_mf_c(demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq);
void demod_mf_pie(demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq);
void demod_mf5_c(const demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq);

/* The PIE matched filter at 5 Msym/s works on aligned groups of five symbols that share a
 * window: groups windows from x (I) and x + DEMOD_STRIDE (Q), advancing 16 samples a group,
 * against the five 32-byte coefficient sets at sets, outputs five per group. */
void demod_mf5_pie(const int8_t *x, const int8_t *sets, unsigned groups, int16_t *oi, int16_t *oq);

/* sum[0] += oi[0..n), sum[1] += oq[0..n); n a multiple of 8, both 16-byte aligned. */
void demod_sum_c(const int16_t *oi, const int16_t *oq, unsigned n, int32_t *sum);
void demod_sum_pie(const int16_t *oi, const int16_t *oq, unsigned n, int32_t *sum);

/* The search's integrate-and-dump: h[k] = the sum of the even samples among x[8k .. 8k + 7]
 * for n candidates (even), x 16-byte aligned; w: 32 bytes, 1 at even bytes 0..7, then at even
 * bytes 8..15 of a 16-byte vector. */
void demod_halves_c(const int8_t *xi, const int8_t *xq, unsigned n, int16_t *hi, int16_t *hq,
                    const int8_t *w);
void demod_halves_pie(const int8_t *xi, const int8_t *xq, unsigned n, int16_t *hi, int16_t *hq,
                      const int8_t *w);

/* The search's signs for 64 candidates: a_k = h[k] + h[k + 1] per axis (h: 72 entries,
 * 16-byte aligned, h[0] the previous block's last half sum), and bit 63 - k of the result is
 * set when Im(a_k conj(a_{k-2})) = aq_k ai_{k-2} - ai_k aq_{k-2} < 0. a (16-byte aligned) holds
 * a_{-2}, a_{-1} in lanes 6, 7 of I (a[0..7]) and Q (a[8..15]) and returns a_56 .. a_63;
 * sums gain h[1..64]. scratch: 512 bytes, 16-byte aligned. */
uint64_t demod_signs_c(const int16_t *hi, const int16_t *hq, int16_t *a, int32_t *sums,
                       int16_t *scratch);
uint64_t demod_signs_pie(const int16_t *hi, const int16_t *hq, int16_t *a, int32_t *sums,
                         int16_t *scratch);

/* The constants for a run with carrier step inc per symbol, leak (lr, li), DC (dr, di) and
 * soft-bit gain. */
void demod_run_init(demod_run_t *k, uint32_t inc, int16_t lr, int16_t li, int16_t dr, int16_t di,
                    int16_t gain);

/* In place, `groups` groups of 8: take the DC off, rotate by exp(-j phase) with phase
 * stepping inc per symbol (the group's base from the table at phase >> 22, phase += step8
 * per group, lanes by co/so), then take the leak off. */
void demod_turn_c(int16_t *yi, int16_t *yq, unsigned groups, uint32_t phase, uint32_t step8,
                  const demod_run_t *k);
void demod_turn_pie(int16_t *yi, int16_t *yq, unsigned groups, uint32_t phase, uint32_t step8,
                    const demod_run_t *k);

/* Where the soft kernels write, at the run's first symbol: hard and weak bits MSB first, and
 * soft bits (C only, NULL for none). QPSK uses plane 0, two bits a symbol; 8PSK writes symbol
 * bit i to plane i, one bit a symbol. */
typedef struct {
    uint8_t *hard[3], *weak[3];
    int8_t *llr[3];
} demod_out_t;

/* n derotated symbols (QPSK: a multiple of 4; 8PSK: of 8, rotated a further -pi/8):
 * PL descrambling by the 2-bit codes in scr (8 symbols per 16 bits, 2-byte aligned), soft bits
 * s = mul(distance, gain) as in rx_frame_t, hard bit s < 0, weak bit |s| < 16, soft bits
 * clip(s, +-127). Descrambling multiplies by j^-R. 8PSK: s1 from x, s2 from y, s0 from
 * |x| - |y| with gain0. */
void demod_qpsk_c(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                  const demod_run_t *k, const demod_out_t *o);
void demod_qpsk_pie(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                    const demod_run_t *k, const demod_out_t *o);
void demod_8psk_c(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                  const demod_run_t *k, const demod_out_t *o);
void demod_8psk_pie(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                    const demod_run_t *k, const demod_out_t *o);

#endif
