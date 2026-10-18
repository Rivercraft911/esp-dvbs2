#pragma once
#include <stdint.h>

// Preallocated PSRAM records, with one internal cursor per owning core.
// A null cursor is dormant: callers must skip the recorder entirely.
typedef struct stream_trace stream_trace_t;
typedef struct stream_trace_event stream_trace_event_t;
typedef struct {
    stream_trace_event_t *next, *end;
    uint32_t dropped;
} stream_trace_lane_t;
enum {
    TRACE_FEC_START = 1, TRACE_FEC_END, TRACE_MAP_WAIT_START, TRACE_MAP_WAIT_END,
    TRACE_PL_WAIT_START, TRACE_PL_WAIT_END, TRACE_PUBLISH, TRACE_ADMIT,
    TRACE_RELEASE, TRACE_BOUND, TRACE_DATA_END, TRACE_GAP_MAX, TRACE_FRAME_START
};
#define STREAM_TRACE_FIRST 64u
#define STREAM_TRACE_FRAMES 64u
stream_trace_t *stream_trace_new(void);
void stream_trace_free(stream_trace_t *);
void stream_trace_arm(stream_trace_t *, stream_trace_lane_t *, unsigned lane);
void stream_trace_finish(stream_trace_t *, stream_trace_lane_t *, unsigned lane);
// Active cursor only. One CCOUNT read, six record stores and a cursor advance.
void stream_trace_event(stream_trace_lane_t *, unsigned event, uint32_t frame,
                        uint32_t put, uint32_t get, uint32_t a, uint32_t b);
void stream_trace_print(const stream_trace_t *);
