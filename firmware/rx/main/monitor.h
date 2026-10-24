#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "dvbs2rx.h"

/* The receiver's half of the monitor stream (mon_pack.h; eth_egress.c sends it). receive.c
 * publishes the run's demodulator and deframer and each frame's MER, carrier and clock; the
 * sender copies samples (rx_demod_snapshot) and symbols (rx_demod_tap) itself, on core 0. Work
 * on core 1, or in the release beyond a memcpy, cost the receiver frames (one copy per 100 ms on
 * core 1: 322 runs dropped a minute). */
typedef struct {
    rx_demod_t *volatile demod;     /* the run's, from its start to its finish; else NULL */
    rx_ts_t *volatile ts;
    volatile int16_t mer_cdb;       /* the last frame's */
    volatile int32_t cfo_hz, clock_ppb;
    volatile uint32_t frames;       /* frames seen, so the sender knows a new one came */
} rx_monitor_t;
