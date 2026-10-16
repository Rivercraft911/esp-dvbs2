/* Shared between dvbs2.c (reference stages, generated tables) and fast.c. */
#ifndef DVBS2_PRIVATE_H
#define DVBS2_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

/* 90 pi/2-BPSK PLHEADER phases (5.5.2). */
void dvbs2_plheader(unsigned modcod, int pilots, uint8_t *output);
/* Byte-at-a-time BCH remainder table, most significant 32-bit word first. */
const uint32_t (*dvbs2_bch_bytes(unsigned t))[6];
/* Per Annex B row: degree, then (x % q) << 9 | x / q for each address. */
const uint16_t *dvbs2_ldpc_table(unsigned modcod);
/* BB scrambling sequence, one byte per 8 BBFRAME bits (5.2.2). */
const uint8_t *dvbs2_prbs_table(void);
/* PL scrambling rotation R(k), 0..3, for code 0 (5.5.4). */
unsigned dvbs2_pl_rotation(size_t k);

#endif
