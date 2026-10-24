#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "dvbs2rx.h"

/* The receiver's half of the monitor stream (mon_pack.h; eth_egress.c sends it). receive.c
 * publishes the run's demodulator and deframer and each frame's MER, carrier and clock, and
 * core 1 copies a snapshot of the newest dump samples when the sender asks: a memcpy from the
 * demodulator's own int8 ring after a capture unit, at most ten times a second. */
typedef struct {
    rx_demod_t *volatile demod;     /* the run's, from its start to its finish; else NULL */
    rx_ts_t *volatile ts;
    volatile bool want_samples;     /* the sender asks ... */
    volatile bool have_samples;     /* ... core 1 has copied MON_SNAPSHOT samples to si, sq */
    int8_t *si, *sq;
    uint32_t copies, copy_cycles, copy_max;   /* core 1's copies and their cost */
    volatile int16_t mer_cdb;       /* the last frame's */
    volatile int32_t cfo_hz, clock_ppb;
    volatile uint32_t frames;       /* frames seen, so the sender knows a new one came */
} rx_monitor_t;
