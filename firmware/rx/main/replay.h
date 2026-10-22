#pragma once

#include <stddef.h>
#include <stdint.h>
#include "dvbs2rx.h"
#include "esp_err.h"

#define REPLAY_LOG 256
typedef struct {
    uint32_t log[REPLAY_LOG];   /* cycles of the first pushes */
    uint32_t pushes, frames, good, failed, max_push_cycles, max_fec_cycles;
    uint64_t push_cycles, fec_cycles, max_push_at;
    rx_demod_stats_t demod;
} replay_stats_t;

/* Read the next `bytes` from USB into PSRAM: int8 I, Q pairs at 16 MS/s (.sc8). */
void replay_upload(size_t bytes);
/* Run the stored capture through the demodulator and FEC, 6,144 words per push. */
esp_err_t replay_run(unsigned modcod, replay_stats_t *stats);
