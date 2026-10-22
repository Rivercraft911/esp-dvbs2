#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RRC4_TILE_PAIRS 512U
/* Fixed4MS SC8 ->1MS S16Q6,65 real Q15 taps. This is the collector's
 * qualification consumer, not the fractional rx_filter or a timing loop.
 * All buffers disjoint; vector calls require caller-owned QR/SAR_BYTE/ACCX
 * and one bounded IRQ mask. No PIE state survives a return. */
typedef struct {
    _Alignas(16) int16_t plane[4][RRC4_TILE_PAIRS+64];
    int8_t carry[64][2];uint64_t total;unsigned phase;
} rrc4_tile_partial;
bool rrc4_tile_partial_init(rrc4_tile_partial *,unsigned);
bool rrc4_tile_partial_push(rrc4_tile_partial *,const int8_t *,size_t,int16_t *,size_t,size_t *,bool);

typedef struct {size_t prefix,groups,tail;} rrc4_unpack_span;
/* Pure planning for an exact SC8 span; plane stores stay inside [first,first+n).
 * Scalar prefix/tail avoid physical VLD128 under/overread. No state mutation. */
void rrc4_unpack_plan(uintptr_t input,unsigned first,unsigned length,size_t pairs,
                     rrc4_unpack_span *);
/* All four S16 destinations16aligned; reverse pointers address the first
 * eight-pair group's LOW address. Caller proves all physical source loads lie
 * in its input span. Caller owns q/SAR_BYTE and masks IRQs. CPE restored; scalar
 * SAR/QACC/ACCX untouched. Each group consumes16SC8 bytes and writes64S16bytes. */
void rrc4_unpack8(const int8_t *,int16_t *,int16_t *,int16_t *,int16_t *,size_t);

/* At most128 selected outputs per512-input tile. Forward pointers advance4
 * S16s/output; reversed pointers retreat4. Allwindows have65 real pairs and
 * the same physical floor16/SRCcontext bounds as qualified rrc4_sym32.
 * I/Qplane phases match. Distinct out is S16aligned and holds2*count samples.
 * Caller masks IRQs and owns q/SAR_BYTE/ACCX; only CPE is restored locally.
 * count0 touches no pointer/context. No vectorstate lives across return. */
void rrc4_batch32(const int16_t *fi,const int16_t *ri,
                  const int16_t *fq,const int16_t *rq,int16_t *out,size_t count);
