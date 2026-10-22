#pragma once

#include <stdint.h>
#include "capture.h"
#include "dvbs2rx.h"

/* One binary record per frame on USB, followed by length payload bytes when
 * length > 0. A record with length RECEIVE_END closes the stream. */
#define RECEIVE_MAGIC 0x46524453U   /* "SDRF" */
#define RECEIVE_END (-2)
typedef struct __attribute__((packed)) {
    uint32_t magic, sequence;
    int32_t cfo_hz;
    int16_t mer_cdb, length;        /* payload bytes, or -1 if the frame failed */
    uint16_t iterations, failed_checks;
} receive_record_t;

typedef struct {
    uint32_t frames, good, failed, dropped, max_iterations;
    uint32_t max_push_cycles, max_fec_cycles;
    uint64_t push_cycles, fec_cycles;
    rx_demod_stats_t demod;
} receive_stats_t;

esp_err_t receive_run(const capture_config_t *capture, unsigned modcod, receive_stats_t *stats,
                      capture_stats_t *capture_stats);
