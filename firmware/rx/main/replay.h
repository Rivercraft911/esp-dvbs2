#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "receive.h"

/* Read the next `bytes` from USB into the PSRAM capture store: int8 I, Q pairs at 16 MS/s (.sc8). */
void replay_upload(size_t bytes);
/* Run the stored capture through the receiver with the radio off (receive_replay). */
esp_err_t replay_run(unsigned modcod, unsigned rate, unsigned skip, const receive_options_t *options,
                     receive_stats_t *stats, capture_stats_t *capture_stats);
