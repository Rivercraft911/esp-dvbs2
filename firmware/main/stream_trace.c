#include "stream_trace.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define PRODUCER_EVENTS 12288u
#define FILL_EVENTS 8192u

typedef struct {
    uint32_t us, cycles, frame, event, put, get, a, b;
} event_t;
typedef struct {
    unsigned used, dropped;
    uint32_t max_cycles, dummies;
    uint64_t cycles;
} lane_t;
struct stream_trace {
    uint32_t epoch_us;
    lane_t lane[2];
    event_t producer[PRODUCER_EVENTS], fill[FILL_EVENTS];
};

stream_trace_t *stream_trace_new(void)
{
    stream_trace_t *t = heap_caps_malloc(sizeof *t,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!t) return NULL;
    // Touch all storage before either worker runs. No allocation or printing
    // in the fill, no shared write cursor and no per-page kernel calls.
    memset(t,0,sizeof *t);
    t->epoch_us = (uint32_t)esp_timer_get_time();
    return t;
}

void stream_trace_free(stream_trace_t *t) { heap_caps_free(t); }

void stream_trace_event(stream_trace_t *t, unsigned lane, unsigned event, uint32_t frame,
                        uint32_t put, uint32_t get, uint32_t a, uint32_t b)
{
    if (event != TRACE_BOUND && event != TRACE_GAP_MAX && frame-STREAM_TRACE_FIRST >= STREAM_TRACE_FRAMES) return;
    const uint32_t before = esp_cpu_get_cycle_count();
    lane_t *l = &t->lane[lane];
    unsigned capacity = lane ? FILL_EVENTS : PRODUCER_EVENTS;
    if (l->used == capacity) { ++l->dropped; return; }
    if (event == TRACE_ADMIT) {
        if (a & (1u<<19)) ++l->dummies;
        else { b = l->dummies; l->dummies = 0; }
    }
    event_t *e = (lane ? t->fill : t->producer)+l->used++;
    // IDF's SYSTIMER clock is shared by both cores. CCOUNT is local to its
    // core and is supplied separately, never used to align the two lanes.
    *e = (event_t){(uint32_t)esp_timer_get_time()-t->epoch_us,
                   before,frame,event,put,get,a,b};
    uint32_t cycles = esp_cpu_get_cycle_count()-before;
    l->cycles += cycles;
    if (cycles > l->max_cycles) l->max_cycles = cycles;
}

void stream_trace_print(const stream_trace_t *t)
{
    if (!t) return;
    unsigned ended = 0;
    for (unsigned i = 0; i < t->lane[1].used; ++i)
        ended += t->fill[i].event == TRACE_DATA_END;
    printf(",\"stream_trace\":{\"version\":1,\"first_frame\":%u,\"frame_count\":%u,"
           "\"frames_complete\":%u,\"complete\":%s,\"storage\":\"psram\",\"bytes\":%u,"
           "\"clock\":\"shared esp_timer microseconds since epoch_us; cycles are core-local CCOUNT\","
           "\"epoch_us\":%u,\"single_page_batches\":true,\"hook_timing_scope\":\"event body only, excludes call/filter/return\","
           "\"columns\":[\"us\",\"cycles\",\"frame\",\"event\",\"produced\",\"consumed\",\"a\",\"b\"],"
           "\"events\":{\"fec_start\":1,\"fec_end\":2,\"map_wait_start\":3,\"map_wait_end\":4,"
           "\"pl_wait_start\":5,\"pl_wait_end\":6,\"publish\":7,\"admit\":8,\"release\":9,"
           "\"bound\":10,\"data_end\":11,\"gap_max\":12,\"frame_start\":13},\"lanes\":[",
           STREAM_TRACE_FIRST,STREAM_TRACE_FRAMES,ended,
           ended == STREAM_TRACE_FRAMES && !t->lane[0].dropped && !t->lane[1].dropped ? "true" : "false",
           (unsigned)sizeof *t,(unsigned)t->epoch_us);
    for (unsigned lane = 0; lane < 2; ++lane) {
        const lane_t *l = &t->lane[lane];
        const event_t *events = lane ? t->fill : t->producer;
        printf("%s{\"core\":%u,\"dropped\":%u,\"hook_body_cycles\":%"PRIu64",\"hook_body_max_cycles\":%u,\"rows\":[",
               lane ? "," : "",lane,l->dropped,l->cycles,(unsigned)l->max_cycles);
        for (unsigned i = 0; i < l->used; ++i) {
            const event_t *e = events+i;
            printf("%s[%u,%u,%u,%u,%u,%u,%u,%u]",i ? "," : "",
                   (unsigned)e->us,(unsigned)e->cycles,(unsigned)e->frame,(unsigned)e->event,
                   (unsigned)e->put,(unsigned)e->get,(unsigned)e->a,(unsigned)e->b);
        }
        printf("]}");
    }
    printf("]}");
}
