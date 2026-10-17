#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATION_CALL_MAX 23
/* Timing in samples at STATION_RATE. At twice that rate (80 MS/s) every duration doubles, so
 * the ID stays 20 WPM; the tone keeps its 40-sample period and moves to +2 MHz. */
#define STATION_RATE 40000000U
#define STATION_DOT 2400000U     /* PARIS: 1.2 / 20 WPM = 60 ms */
#define STATION_EDGE 200000U     /* 5 ms raised cosine, quantized to 10 us */
#define STATION_CELL 400U        /* 10 us envelope step */
#define STATION_PERIOD (540ULL * STATION_RATE) /* 9 minutes, including ID */
#define STATION_HEADER_SIZE 40
#define STATION_MARKS 144       /* 24 chars * at most six Morse marks */

typedef int (*station_fill_fn)(uint32_t *, uint64_t, size_t, void *);
typedef struct {
    uint32_t start, end;
} station_mark_t;
typedef struct {
    station_mark_t marks[STATION_MARKS];
    unsigned nmarks;
    uint32_t length;
    uint64_t payload_samples, total_samples;
    station_fill_fn payload;
    void *ctx;
    int16_t sine[40], cosine[40];
    uint8_t edge[501];
    uint32_t scale;              /* DAC rate / STATION_RATE: 1 or 2 */
} station_id_t;

/* Syntax only, never proof of an assignment. Uppercase, 3..23 ASCII characters,
 * letters/digits and single slashes, at least one letter and digit. */
bool station_callsign(const char *input, char out[STATION_CALL_MAX + 1]);
bool station_id_init(station_id_t *id, const char *call, uint64_t payload_samples,
                     station_fill_fn payload, void *ctx);
/* The same for a DAC at `rate` samples per second: STATION_RATE or 2 * STATION_RATE. */
bool station_id_init_rate(station_id_t *id, const char *call, uint64_t payload_samples,
                          uint32_t rate, station_fill_fn payload, void *ctx);
/* Idempotent at arbitrary sample indices, including bank overlap rereads. */
int station_id_fill(uint32_t *dst, uint64_t first, size_t count, void *ctx);
/* Same CW generator alone, used by the radio-off dump. */
int station_cw_fill(uint32_t *dst, uint64_t first, size_t count, void *ctx);
/* Replace the first 40 bytes of EVERY GCS payload frame, leaving the seeded
 * generator's advancement unchanged. No header when call is empty. */
void station_payload_header(uint8_t *payload, size_t bytes, const char *call);
