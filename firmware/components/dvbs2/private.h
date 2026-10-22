/* Shared between dvbs2.c (reference stages, generated tables) and fast.c. */
#ifndef DVBS2_PRIVATE_H
#define DVBS2_PRIVATE_H

#include <stddef.h>
#include <stdint.h>
#include "dvbs2.h"

/* 90 pi/2-BPSK PLHEADER phases (5.5.2). */
void dvbs2_plheader(unsigned modcod, int short_frame, int pilots, uint8_t *output);
/* The table entry with the same modcod and frame size, or NULL: a mode is valid only by identity. */
const dvbs2_mode *dvbs2_canonical_mode(const dvbs2_mode *mode);
/* FECFRAME length in bits: 64,800 normal, 16,200 short. */
static inline unsigned dvbs2_fec_bits(const dvbs2_mode *mode)
{
    return mode->short_frame ? 16200 : 64800;
}
/* BB scrambling sequence, one byte per 8 BBFRAME bits (5.2.2). */
const uint8_t *dvbs2_prbs_table(void);

#endif
