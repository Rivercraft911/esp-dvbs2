#ifndef RX_FEC_INTERNAL_H
#define RX_FEC_INTERNAL_H

/* Workspace layout shared by rx_fec.c, the PIE kernels and the bench. */
#include <stddef.h>
#include <stdint.h>

#include "dvbs2.h"

#define RX_GROUPS 45            /* 16,200 / 360 column groups, information then parity */
#define RX_VECTORS 23           /* 360 checks of a row group in 23 x 16 lanes */
#define RX_SLOT 384             /* one group: 360 beliefs, 0..15 again at 360..375 */
#define RX_MAX_Q 25
#define RX_SCALE 112            /* message = min(min * 112 / 128, 47) (normalized min-sum) */
#define RX_OFFSET 0
#define RX_RMAX 47              /* well below 127, so q = b - R stays large when b saturates */
#define RX_UNITS 3              /* integer LLR units per natural LLR unit after loading */
#define RX_LMAX 63              /* channel clip: two checks (2 x 47) can overturn any bit */

typedef struct {
    uint16_t home;      /* byte offset of the group's slot from belief */
    uint16_t write;     /* where pass 2 writes: the slot, or a scratch slot for a repeat */
    uint16_t shift;     /* check t of the row group reads bit (t - shift) mod 360 */
    uint16_t previous;  /* repeat: shift of the group's previous edge in this row group */
    uint8_t group, repeat;
} rx_edge_t;

struct rx_fec {
    unsigned modcod, kbch, nbch, q, groups, edge_count, most, max_iterations;
    int scale, offset, rmax, lmax;
    unsigned missing_edge, missing_read;
    int8_t *belief, *r, *qtmp;
    uint16_t *reads;    /* per layer, vector, edge: window start as a belief offset */
    rx_edge_t *edges;
    uint8_t *prbs;
    dvbs2_fast *encoder;        /* clean check: the transmitter's fast BCH and LDPC */
    dvbs2_workspace *bits;      /* DVBS2_SHORT_FAST_WORKSPACE_USED bytes of it */
    uint16_t layer_first[RX_MAX_Q];
    uint8_t layer_count[RX_MAX_Q];
    uint16_t frame[RX_GROUPS];  /* each slot's frame at the start of an iteration */
    int8_t state[RX_VECTORS * 64] __attribute__((aligned(16)));  /* min1, min2, q and b signs */
};

#endif
