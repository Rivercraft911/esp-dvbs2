#ifndef DVBS2RX_DEMOD_PIE_H
#define DVBS2RX_DEMOD_PIE_H

#include <stdint.h>

/* The demodulator's release kernels. The portable versions in demod.c define the results;
 * demod_pie.S gives the same bytes on the S3. Arithmetic is that of the PIE lanes:
 * mul(a, b) = the low 16 bits of floor(a b / 2^14), and sums saturate to int16.
 *
 * Data symbols sit in planar int16 rings (I and Q), 16-byte aligned, 8 symbols a group. */
typedef struct {
    int16_t co[8], so[8];   /* cos and sin of k inc for lane k, Q14 */
    int16_t lr[8], li[8];   /* carrier leak after derotation, every lane */
    int16_t dr[8], di[8];   /* receiver DC, every lane */
    int16_t sel[2][8];      /* lane k: 1 << 2k and 1 << (2k + 1), the scrambling code bits */
    int16_t gain[8];        /* LLR scale, Q14, every lane */
    int16_t hi[8], lo[8];   /* 127, -127 */
    int16_t weight[2][8];   /* lane k: hard bit 15 - k and 7 - k */
    int16_t one[8];
    const int16_t *table;   /* sin(2 pi k / 1024), Q14 */
} __attribute__((aligned(16))) demod_run_t;  /* the PIE code relies on this layout */

/* Matched filter at the tracked times. Output k at time t + k step (Q24 dump samples, with
 * 2^21 added to round to a quarter sample): the 112-byte coefficient set for its offset and
 * phase against both rings from the 16-aligned base (demod_tables.h), each sum >> 4, stored
 * as planar int16 and added to sum. t advances by n steps. */
#define DEMOD_RING 4096u        /* sample ring per axis */
#define DEMOD_STRIDE 4352u      /* rq - ri: the ring and its mirrored head, 17 x 256 */
typedef struct {
    const int8_t *ri, *rq;      /* sample rings, 16-byte aligned */
    const int8_t *sets;         /* rx_mf_sets */
    uint32_t t_lo, t_hi;
    uint32_t step;
    int32_t sum_i, sum_q;
} demod_mf_t;                   /* the PIE code relies on this layout */

void demod_mf_c(demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq);
void demod_mf_pie(demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq);

/* The search's integrate-and-dump: h[k] = the sum of the even samples among x[8k .. 8k + 7]
 * for n candidates (even), x 16-byte aligned; w: 32 bytes, 1 at even bytes 0..7, then at even
 * bytes 8..15 of a 16-byte vector. */
void demod_halves_c(const int8_t *xi, const int8_t *xq, unsigned n, int16_t *hi, int16_t *hq,
                    const int8_t *w);
void demod_halves_pie(const int8_t *xi, const int8_t *xq, unsigned n, int16_t *hi, int16_t *hq,
                      const int8_t *w);

/* The constants for a run with carrier step inc per symbol, leak (lr, li) and LLR gain. */
void demod_run_init(demod_run_t *k, uint32_t inc, int16_t lr, int16_t li, int16_t dr, int16_t di,
                    int16_t gain);

/* In place, `groups` groups of 8: take the DC off, rotate by exp(-j phase) with phase
 * stepping inc per symbol (the group's base from the table at phase >> 22, phase += step8
 * per group, lanes by co/so), then take the leak off. */
void demod_turn_c(int16_t *yi, int16_t *yq, unsigned groups, uint32_t phase, uint32_t step8,
                  const demod_run_t *k);
void demod_turn_pie(int16_t *yi, int16_t *yq, unsigned groups, uint32_t phase, uint32_t step8,
                    const demod_run_t *k);

/* Where demod_llr writes: LLRs (16-byte aligned) and hard bits of the first symbol. */
typedef struct {
    int8_t *llr;
    uint8_t *hard;
} demod_out_t;

/* n symbols (a multiple of 4): PL descrambling by the 2-bit codes in scr (8 symbols per 16
 * bits, 2-byte aligned), LLRs = clip(mul(x, gain), +-127) in codeword order, hard bits = LLR
 * signs, MSB first. */
void demod_llr_c(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                 const demod_run_t *k, const demod_out_t *o);
void demod_llr_pie(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                   const demod_run_t *k, const demod_out_t *o);

#endif
