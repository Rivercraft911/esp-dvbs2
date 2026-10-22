#ifndef RX_FEC_INTERNAL_H
#define RX_FEC_INTERNAL_H

/* Workspace layout and kernels shared by rx_fec.c, rx_fec_pie.S and the bench. The
 * assembler sees only the constants. */

#define RX_GROUPS 45            /* 16,200 / 360 column groups, information then parity */
#define RX_VECTORS 23           /* 360 checks of a row group in 23 x 16 lanes */
#define RX_SLOT 384             /* one group: 360 beliefs, 0..15 again at 360..375 */
#define RX_MAX_Q 25
#define RX_SCALE 112            /* message = min(min * 112 / 128, 47): normalized min-sum */
#define RX_RMAX 47              /* well below 127, so q = b - R stays large when b saturates */
#define RX_UNITS 3              /* integer LLR units per natural LLR unit after loading */
#define RX_LMAX 31              /* channel clip below RX_RMAX: one check can overturn a bit */
#define RX_STAT_VECTORS 1012    /* LLR statistics over the first 16,192 values */
#define RX_ENCODER_TAIL 1664    /* resident LDPC program, columns and CRC (1,600 at most) */

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#include "dvbs2.h"

typedef struct {
    uint16_t home;      /* byte offset of the group's slot from belief */
    uint16_t shift;     /* check t of the row group reads bit (t - shift) mod 360 */
    uint16_t previous;  /* repeat: shift of the group's previous edge in this row group */
    uint8_t group, repeat;
} rx_edge_t;

/* Per row group, edges are first occurrences of their groups, then repeats; every
 * per-edge array below follows that order, row group after row group. state holds
 * min1, min2 and the q and b sign parities of each vector of the current layer. */
struct rx_fec {
    unsigned modcod, kbch, nbch, q, groups, edge_count, most, max_iterations, pie;
    unsigned missing_edge, missing_read;
    int8_t *belief, *r, *qtmp, *state;  /* work area; belief is NULL without one */
    uint16_t *reads;    /* per layer, vector, edge: window start as a belief offset */
    uint16_t *writes;   /* per edge: where pass 2 writes (the slot, or scratch for repeats) */
    uint16_t *checks;   /* per edge: slot and window start at the start of an iteration */
    rx_edge_t *edges;
    uint8_t *prbs;
    dvbs2_fast *encoder;        /* clean check: the transmitter's fast BCH and LDPC */
    dvbs2_workspace *bits;      /* DVBS2_SHORT_FAST_WORKSPACE_USED bytes of it */
    uint16_t layer_first[RX_MAX_Q];
    uint8_t layer_count[RX_MAX_Q], layer_repeats[RX_MAX_Q];
    uint16_t frame[RX_GROUPS];  /* each slot's frame at the start of an iteration */
    uint8_t crc8[256];          /* BBHEADER CRC-8 by byte */
};

/* Kernels. Each has a portable version (rx_fec.c) and, on the S3, a PIE version with
 * the suffix _pie (rx_fec_pie.S) that gives identical bytes. Buffers are 16-byte
 * aligned unless noted.
 *
 * pass1: for 23 vectors of 'count' edges, window b at belief + reads[v][e] (any
 *   alignment), q = sat(b - r[v][e]); qtmp[v][e] = |q| (at most 127), then q; state[v] =
 *   min1, min2 of |q|, XOR of q, XOR of b; then 16 bytes counting, per lane, the vectors
 *   whose XOR of b is negative. Reads one more reads entry, r vector and window than it
 *   uses.
 * pass2: messages and beliefs. edges = first | repeats << 8. For each vector,
 *   n = min(m * RX_SCALE >> 7, RX_RMAX) of both minima, signed by the q parity;
 *   message = sign(q) * (|q| < min2 ? n2 : n1). r[v][e] = message; the belief at
 *   belief + writes[e] + 16 v is sat(q + message), or sat(message - old r) for repeats.
 *   Reads 32 bytes past qtmp.
 * tails: positions 360..375 of 'count' slots repeat 0..15 (slot offsets in writes).
 * repeat: scratch = sat(home window at start + scratch) over 23 vectors, then home =
 *   scratch, then its tail.
 * scale: in 'slots' slots of 384 bytes, x = sign(x) * min(RX_LMAX, min(|x|, 127) * gain >> 4).
 * moments: sums of min(|x|, 127) and its square over 16 * vectors bytes at any alignment.
 * parity: XOR of 'count' windows, edge e at belief + checks[2e] + (checks[2e + 1] + 16 v)
 *   mod 360 for vector v; nonzero if any check of lanes 0..359 has odd parity.
 * hard: 'groups' slots, slot g read from position frames[g], to packed sign bits
 *   (45 bytes per group, one more byte scribbled after the last).
 * xor: x ^= prbs over 16 * vectors bytes. */
void rx_pass1(const int8_t *belief, const uint16_t *reads, const int8_t *r, int8_t *qtmp,
              int8_t *state, unsigned count);
void rx_pass2(int8_t *belief, const uint16_t *writes, int8_t *r, const int8_t *qtmp,
              const int8_t *state, unsigned edges);
void rx_tails(int8_t *belief, const uint16_t *writes, unsigned count);
void rx_repeat(int8_t *home, int8_t *scratch, unsigned start);
void rx_scale(int8_t *belief, unsigned slots, unsigned gain);
void rx_moments(const int8_t *llr, unsigned vectors, uint32_t sums[2]);
int rx_parity(const int8_t *belief, const uint16_t *checks, unsigned count);
void rx_hard(const int8_t *belief, const uint16_t *frames, unsigned groups, uint8_t *hard);
void rx_xor(uint8_t *x, const uint8_t *prbs, unsigned vectors);
void rx_pass1_pie(const int8_t *belief, const uint16_t *reads, const int8_t *r, int8_t *qtmp,
                  int8_t *state, unsigned count);
void rx_pass2_pie(int8_t *belief, const uint16_t *writes, int8_t *r, const int8_t *qtmp,
                  const int8_t *state, unsigned edges);
void rx_tails_pie(int8_t *belief, const uint16_t *writes, unsigned count);
void rx_repeat_pie(int8_t *home, int8_t *scratch, unsigned start);
void rx_scale_pie(int8_t *belief, unsigned slots, unsigned gain);
void rx_moments_pie(const int8_t *llr, unsigned vectors, uint32_t sums[2]);
int rx_parity_pie(const int8_t *belief, const uint16_t *checks, unsigned count);
void rx_hard_pie(const int8_t *belief, const uint16_t *frames, unsigned groups, uint8_t *hard);
void rx_xor_pie(uint8_t *x, const uint8_t *prbs, unsigned vectors);
#endif /* __ASSEMBLER__ */

#endif
