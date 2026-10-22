#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rx_halfband.h"
enum {HB_CONTEXT_WORDS=56,HB_CONTEXT_CASES=72};
typedef struct {
    uint32_t cases,errors,first_leaf,first_trial,forced_calls,raw_difference_cases;
    bool complete;
    /* Native forced-spill evidence proved only natural PS.OWB differences.
     * mismatch_words excludes PS.CALLINC and OWB. case_words retains OWB;
     * first_leaf/trial and snapshots identify the first raw difference.
     * Raw snapshots and XORs retain every PS bit. */
    uint32_t mismatch_words[2],case_words[HB_CONTEXT_CASES][2];
    uint32_t xor_words[HB_CONTEXT_WORDS];
    uint32_t first_before[HB_CONTEXT_WORDS],first_after[HB_CONTEXT_WORDS];
} hb_context_report;
/* Radio must be stopped. raw is one reserved64KiB bank, scratch the existing
 * internal workspace. No allocation, driver, RF or register-bus operation. */
bool hb_context_controls(uint32_t *raw,hb_vector_scratch *scratch,hb_context_report *);

#include "rrc.h"

enum { RRC_PARTIAL_CASES=192, RRC_PARTIAL_COMPONENTS=32640,
       RRC_PARTIAL_INPUT_BYTES=1088 };
#define RRC_PARTIAL_EXPECTED_CRC UINT32_C(785216665)
typedef struct {
    uint32_t cases,components,crc32,errors,first_case,first_component;
    int32_t expected,actual;
    bool measured,guards,context,reset;
} rrc_partial_report;
typedef bool (*rrc_partial_push)(void *,const int8_t *,size_t,size_t *);
typedef bool (*rrc_partial_guard)(void *);
typedef void (*rrc_partial_pause)(void *);

/* Separate guarded input, state and output are borrowed only during this call.
 * The push callback masks one bounded push and returns with IRQs/CPE restored.
 * Guard checks, scalar equations and pause all execute outside that mask. */
bool rrc_partial_controls(rrc4_tile_partial *,uint8_t input[RRC_PARTIAL_INPUT_BYTES],
                          int16_t output[256],rrc_partial_push,rrc_partial_guard,
                          rrc_partial_pause,void *,rrc_partial_report *);
