#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "capture.h"
#include "dvbs2rx.h"
#include "monitor.h"
#include "ts_egress.h"

/* One binary record per frame on USB. A good frame's record (length = payload bytes) is
 * followed by the payload, or at 5 Msym/s by its 4-byte CRC-32 (zlib.crc32, little-endian), as
 * the stream header's "payload" says. A failed frame (-1) is followed by its 8-byte header sample
 * index, then its RX_HARD_BITS hard bits and as many weak bits (codeword order, MSB first). A
 * record with length RECEIVE_END closes the stream. */
#define RECEIVE_MAGIC 0x46524453U   /* "SDRF" */
#define RECEIVE_END (-2)
#define RECEIVE_BITS 2025U          /* 16,200 bits */
typedef struct __attribute__((packed)) {
    uint32_t magic, sequence;
    int32_t cfo_hz;
    int16_t mer_cdb, length;        /* payload bytes, or -1 if the frame failed */
    uint16_t iterations, failed_checks;
} receive_record_t;

/* TS mode: each good frame's data field also goes through the deframer (dvbs2rx.h rx_ts) after its
 * record, and the packets to the egress. pattern: check every packet against the transmitter's
 * test input (firmware/components/ingress/ingress_pattern.c: seven packets per sequence number,
 * which counts on, or loops 0..7 in `spiinput ... ts replay`). */
typedef struct {
    bool ts, pattern, drop_null;
    const ts_egress_t *egress;      /* NULL: packets are only counted */
    rx_monitor_t *mon;              /* the monitor stream's hooks (monitor.h), or NULL */
} receive_options_t;

typedef struct {
    uint32_t checked, bad, jumps, wraps;    /* bad: content differs; jumps: not the next packet */
    uint64_t cycles;
} receive_pattern_t;

#define RECEIVE_LOG 256
typedef struct {
    uint32_t frames, good, failed, dropped, decoded, max_iterations;  /* dropped: no free slot */
    uint32_t max_push_cycles, max_release_cycles, max_fec_cycles;
    uint32_t release_stack_free, check_stack_free, decode_stack_free;
    uint32_t free_internal, largest_internal;   /* after the receiver's allocations */
    uint64_t push_cycles, release_cycles, fec_cycles;
    uint32_t pushes, log[RECEIVE_LOG];  /* cycles per consumer call (a unit, or its part) */
    uint64_t max_push_at;
    rx_demod_stats_t demod;
    rx_ts_stats_t ts;
    rx_ts_pid_t pids[RX_TS_PIDS];
    uint32_t ts_pids, max_ts_cycles;    /* per frame: deframer, pattern check and egress */
    uint64_t ts_cycles;
    receive_pattern_t pattern;
    /* USB: the slowest frame record to write (it blocks while the host does not read), the
     * sequence of that frame, and records that took 1 ms or more. */
    uint32_t max_write_us, max_write_sequence, slow_writes;
} receive_stats_t;

/* The receiver's queues and semaphores, once at boot. */
void receive_init(void);
/* All of count bytes to USB, blocking. */
void receive_write(const void *data, size_t count);
/* Live: capture at capture->mhz, rate 1,000,000 or 5,000,000 symbols/s. */
esp_err_t receive_run(const capture_config_t *capture, unsigned modcod, unsigned rate,
                      const receive_options_t *options, receive_stats_t *stats,
                      capture_stats_t *capture_stats);
/* Radio off: pairs of int8 I, Q at 16 MS/s (.sc8) through the same receiver (capture_replay). */
esp_err_t receive_replay(const int8_t *sc8, size_t pairs, unsigned modcod, unsigned rate,
                         unsigned skip, const receive_options_t *options, receive_stats_t *stats,
                         capture_stats_t *capture_stats);
