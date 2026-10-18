#include "stream_trace.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#ifdef __XTENSA__
#include "sdkconfig.h"
#define CLOCK_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#else
#define CLOCK_MHZ 240u
#endif

#define PRODUCER_EVENTS 12288u
#define FILL_EVENTS 8192u

struct stream_trace_event {
    uint32_t cycles, tag, put, get, a, b;
};
typedef struct {
    uint32_t us, before, after;
} trace_clock_t;
typedef struct {
    trace_clock_t clock;
    unsigned used, dropped, armed;
} lane_t;
struct stream_trace {
    lane_t lane[2];
    stream_trace_event_t producer[PRODUCER_EVENTS], fill[FILL_EVENTS];
};

stream_trace_t *stream_trace_new(void)
{
    stream_trace_t *t = heap_caps_malloc(sizeof *t,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (t) memset(t,0,sizeof *t); // touch storage before playback; no clock reads
    return t;
}

void stream_trace_free(stream_trace_t *t) { heap_caps_free(t); }

void stream_trace_arm(stream_trace_t *t, stream_trace_lane_t *cursor, unsigned lane)
{
    lane_t *l = &t->lane[lane];
    // Once per core, at steady state. The bracket exposes cross-core clock
    // alignment uncertainty; the fixed 240 MHz build has power management off.
    const uint32_t before = esp_cpu_get_cycle_count();
    const uint32_t us = (uint32_t)esp_timer_get_time();
    const uint32_t after = esp_cpu_get_cycle_count();
    l->clock = (trace_clock_t){us,before,after};
    l->armed = 1;
    stream_trace_event_t *begin = lane ? t->fill : t->producer;
    *cursor = (stream_trace_lane_t){begin,begin+(lane ? FILL_EVENTS : PRODUCER_EVENTS),0};
}

void IRAM_ATTR stream_trace_event(stream_trace_lane_t *cursor, unsigned event, uint32_t frame,
                                  uint32_t put, uint32_t get, uint32_t a, uint32_t b)
{
    stream_trace_event_t *e = cursor->next;
    if (e == cursor->end) { ++cursor->dropped; return; }
    e->cycles = esp_cpu_get_cycle_count();
    e->tag = frame | (event<<24);
    e->put = put; e->get = get; e->a = a; e->b = b;
    cursor->next = e+1;
}

void stream_trace_finish(stream_trace_t *t, stream_trace_lane_t *cursor, unsigned lane)
{
    if (!cursor->next) return;
    t->lane[lane].used = cursor->next-(lane ? t->fill : t->producer);
    t->lane[lane].dropped = cursor->dropped;
    cursor->next = NULL;
}

void stream_trace_print(const stream_trace_t *t)
{
    if (!t) return;
    unsigned ended = 0;
    for (unsigned i = 0; i < t->lane[1].used; ++i)
        ended += t->fill[i].tag>>24 == TRACE_DATA_END;
    uint32_t epoch = t->lane[0].armed ? t->lane[0].clock.us : t->lane[1].clock.us;
    if (t->lane[1].armed && (int32_t)(t->lane[1].clock.us-epoch) < 0) epoch = t->lane[1].clock.us;
    printf(",\"stream_trace\":{\"version\":2,\"first_frame\":%u,\"frame_count\":%u,"
           "\"frames_complete\":%u,\"complete\":%s,\"storage\":\"psram\",\"bytes\":%u,"
           "\"clock\":\"core-local CCOUNT; us reconstructed after playback from shared esp_timer anchors\","
           "\"epoch_us\":%u,\"cycles_per_us\":%u,\"event_bytes\":%u,\"single_page_batches\":true,"
           "\"columns\":[\"us\",\"cycles\",\"frame\",\"event\",\"produced\",\"consumed\",\"a\",\"b\"],"
           "\"events\":{\"fec_start\":1,\"fec_end\":2,\"map_wait_start\":3,\"map_wait_end\":4,"
           "\"pl_wait_start\":5,\"pl_wait_end\":6,\"publish\":7,\"admit\":8,\"release\":9,"
           "\"bound\":10,\"data_end\":11,\"gap_max\":12,\"frame_start\":13},\"lanes\":[",
           STREAM_TRACE_FIRST,STREAM_TRACE_FRAMES,ended,
           ended == STREAM_TRACE_FRAMES && !t->lane[0].dropped && !t->lane[1].dropped ? "true" : "false",
           (unsigned)sizeof *t,(unsigned)epoch,CLOCK_MHZ,(unsigned)sizeof(stream_trace_event_t));
    for (unsigned lane = 0; lane < 2; ++lane) {
        const lane_t *l = &t->lane[lane];
        const stream_trace_event_t *events = lane ? t->fill : t->producer;
        const uint32_t base = l->clock.before+(l->clock.after-l->clock.before)/2;
        printf("%s{\"core\":%u,\"armed\":%s,\"dropped\":%u,\"anchor\":{\"us\":%u,"
               "\"cycles_before\":%u,\"cycles_after\":%u},\"rows\":[",
               lane ? "," : "",lane,l->armed ? "true" : "false",l->dropped,
               (unsigned)l->clock.us,(unsigned)l->clock.before,(unsigned)l->clock.after);
        unsigned dummies = 0;
        for (unsigned i = 0; i < l->used; ++i) {
            const stream_trace_event_t *e = events+i;
            unsigned kind = e->tag>>24, frame = e->tag&0xffffff, b = e->b;
            uint32_t us = l->clock.us-epoch+(uint32_t)(e->cycles-base)/CLOCK_MHZ;
            if (kind == TRACE_ADMIT) {
                if (e->a & (1u<<19)) ++dummies;
                else { b = dummies; dummies = 0; }
            }
            printf("%s[%u,%u,%u,%u,%u,%u,%u,%u]",i ? "," : "",
                   (unsigned)us,(unsigned)e->cycles,frame,kind,
                   (unsigned)e->put,(unsigned)e->get,(unsigned)e->a,b);
        }
        printf("]}");
    }
    printf("]}");
}
