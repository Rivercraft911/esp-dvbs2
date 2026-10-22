#pragma once
#include "rx_mc6.h"

/* One correction sweep, with no check-message buffer. The caller owns
 * 129,600 bytes of distinct, 16-byte-aligned beliefs. Tiles can be paused;
 * completion is permanent. Further sweeps require the general decoder,
 * restarted from the original LLRs. Memory placement is the caller's choice. */
typedef struct {
    int16_t *belief;
    uint16_t layer, column;
    bool initialized, complete, failed;
} rx_mc6_first;
bool rx_mc6_first_init(rx_mc6_first *, int16_t belief[RX_MC6_N], const int8_t llr[RX_MC6_N]);
/* Process at most budget tiles, using aligned copies for contiguous spans. */
unsigned rx_mc6_first_step(rx_mc6_first *, unsigned budget);
/* Refuses without publishing unless the one sweep is complete. Full H/BCH/
 * header validation and capacity-checked publication still use rx_mc6_finish. */
bool rx_mc6_first_pack(const rx_mc6_first *, uint8_t bits[RX_MC6_WORD_BYTES]);

#ifdef RX_MC6_TEST_HOOKS
unsigned rx_mc6_first_step_scalar(rx_mc6_first *, unsigned budget);
#endif
