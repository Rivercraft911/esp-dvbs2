#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HB_BANK_PAIRS 16384U
#define HB_MAX_PAIRS 14288U
#define HB_MAX_OUTPUT ((HB_MAX_PAIRS+3U)/4U)
#define HB_SENTINEL UINT32_C(0xa5a5a5a5)
#define HB_TILE 2048U
typedef struct {int16_t i[10],q[10];unsigned held,phase;} hb_history;
typedef struct {hb_history first,second;} hb_tile_state;
typedef struct { int16_t i,q; } hb_complex;
typedef struct {
    _Alignas(16) hb_complex even[1040], odd[1040];
    _Alignas(16) hb_complex second_even[536], second_odd[536];
    _Alignas(16) int8_t final[1056];
} hb_vector_scratch;

/* Leaves own bounded PIE context: PS/CPE/SAR/SAR_BYTE, all QR/QACC/ACCX
 * restored. Callers provide internal operands and documented aligned padding.
 * C callers bound scan<=256, unpack/FIR<=64 groups; no task owns live PIE.
 * Only core1 uses these leaves/counters. */
extern volatile uint32_t hb_leaf_max_cycles, hb_leaf_calls;
extern volatile uint32_t hb_force_spill, hb_forced_spill_calls;
void hb_unpack8(const uint32_t *raw, hb_complex *even, hb_complex *odd, size_t groups);
void hb_fir4_15(const hb_complex *even, const hb_complex *odd,
                 hb_complex *out_even, hb_complex *out_odd, size_t groups);
void hb_fir4_17(const hb_complex *even, const hb_complex *odd, int8_t *out, size_t groups);

/* Two11-tap stages: names15/17 specify arithmetic shifts, not tap counts.
 * State starts zeroed and remains continuous across CPU-owned bank windows.
 * State,scratch,bank,out,written are disjoint; bank is64aligned and scratch
 * is16aligned internal storage. Both raw spans pass sentinel admission before
 * state/output mutation. The caller owns physical bank reuse and output life. */
bool hb_vector_bank(hb_tile_state *state, hb_vector_scratch *scratch, unsigned tile,
                    const uint32_t *bank, unsigned first, size_t pairs,
                    int8_t *out, size_t capacity_pairs, size_t *written_pairs);

/* Bounded native leaves used by prescan, bank clear and radio-off controls. */
bool rx_pack_scan8(const uint32_t *,size_t);
void rx_pack_clear4(uint32_t *,size_t);
void rx_pack8(const uint32_t *,uint8_t *,size_t);
void rx_halfband_clear(uint32_t *);
