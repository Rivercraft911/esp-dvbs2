#pragma once
#include <stdint.h>

// Two single-writer lanes, core 0 producer and core 1 fill. Storage is PSRAM.
// Frame IDs are zero based data frames; dummies do not advance them.
typedef struct stream_trace stream_trace_t;
enum {
    TRACE_FEC_START = 1, TRACE_FEC_END, TRACE_MAP_WAIT_START, TRACE_MAP_WAIT_END,
    TRACE_PL_WAIT_START, TRACE_PL_WAIT_END, TRACE_PUBLISH, TRACE_ADMIT,
    TRACE_RELEASE, TRACE_BOUND, TRACE_DATA_END, TRACE_GAP_MAX, TRACE_FRAME_START
};
#define STREAM_TRACE_FIRST 64u
#define STREAM_TRACE_FRAMES 64u
stream_trace_t *stream_trace_new(void);
void stream_trace_free(stream_trace_t *);
void stream_trace_event(stream_trace_t *, unsigned lane, unsigned event, uint32_t frame,
                        uint32_t put, uint32_t get, uint32_t a, uint32_t b);
void stream_trace_print(const stream_trace_t *);
