/* Shared between dvbs2.c (reference stages, generated tables) and fast.c. */
#ifndef DVBS2_PRIVATE_H
#define DVBS2_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

/* 90 pi/2-BPSK PLHEADER phases (5.5.2). */
void dvbs2_plheader(unsigned modcod, int pilots, uint8_t *output);
/* BB scrambling sequence, one byte per 8 BBFRAME bits (5.2.2). */
const uint8_t *dvbs2_prbs_table(void);

#endif
