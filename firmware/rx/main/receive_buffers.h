#pragma once

#include "dvbs2rx.h"

/* Shared by the receiver's allocations and arena_check(). Add another RX_HARD_BYTES plane
 * here when rx_frame_t gains it: the job storage and copy follow this list. fine is copied
 * only from frames that have it (1 Msym/s, receive.c). */
#define JOBS 3U
#define JOB_PLANES(X) X(hard) X(weak) X(fine)
typedef struct {
    rx_frame_t frame;
#define PLANE(name) uint8_t name[RX_HARD_BYTES] __attribute__((aligned(16)));
    JOB_PLANES(PLANE)
#undef PLANE
} decode_job_t;

/* 8PSK short frames with pilots: (16200/3 + 90 + 3*36) / (5e6 symbols/s) = 1.1196 ms.
 * 12 ms needs 11 frames; 16 gives margin (17.9 ms of frames). The ordering module reserves
 * RX_HARD_BYTES per frame, enough for every short BBHEADER and data field. All in PSRAM. */
#define ORDER_FRAMES 16U
