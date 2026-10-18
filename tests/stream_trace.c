// No recorder clocks before arming; one CCOUNT read per event; bounded storage.
#include "native.h"
static int wrong_heap;
static void *trace_malloc(size_t n, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    void *p = heap_caps_malloc(n,caps);
    if (p && wrong_heap)
        for (unsigned i = 0; i < 64; ++i) if (native_blocks[i].p == p) native_blocks[i].caps = MALLOC_CAP_INTERNAL;
    if (p) memset(p,0xa5,n); // poison to detect unnecessary ring initialization
    return p;
}
#define heap_caps_malloc trace_malloc
static uint32_t cycle_now = UINT32_MAX-500;
static unsigned cycle_reads, time_reads;
static uint32_t trace_cycles(void) { ++cycle_reads; cycle_now += 240; return cycle_now; }
static int64_t trace_time(void) { ++time_reads; return (int64_t)UINT32_MAX+12; }
#define esp_cpu_get_cycle_count trace_cycles
#define esp_timer_get_time trace_time
#include "../firmware/main/stream_trace.c"
#undef esp_timer_get_time
#undef esp_cpu_get_cycle_count
#undef heap_caps_malloc

int main(void)
{
    fail_after = 0; assert(!stream_trace_new(STREAM_TRACE_ALLOCATE,1000,950,54)); fail_after = -1;
    wrong_heap = 1;
    assert(!stream_trace_new(STREAM_TRACE_ALLOCATE,1000,950,54) && !live_allocations);
    wrong_heap = 0;
    stream_trace_t *t = stream_trace_new(STREAM_TRACE_ALLOCATE,1000,950,54);
    stream_trace_lane_t cursor[2] = {0};
    assert(t && live_allocations == 1 && !cycle_reads && !time_reads);
    assert(esp_ptr_external_ram(t));
    assert(t->producer[0].cycles == 0xa5a5a5a5 && t->fill[FILL_EVENTS-1].b == 0xa5a5a5a5);
    assert(t->internal_before == t->internal_after);
    assert(!cursor[0].next && !cursor[1].next);
    // Exactly one common-clock anchor per core. No shared-clock calls per event.
    for (unsigned lane = 0; lane < 2; ++lane) {
        stream_trace_arm(t,cursor+lane,lane);
        unsigned cycles = cycle_reads, times = time_reads;
        stream_trace_event(cursor+lane,TRACE_FRAME_START,STREAM_TRACE_FIRST,UINT32_MAX,UINT32_MAX-1,0,0);
        assert(cycle_reads == cycles+1 && time_reads == times);
        for (unsigned i = 0; i < PRODUCER_EVENTS+1; ++i)
            stream_trace_event(cursor+lane,TRACE_PUBLISH,STREAM_TRACE_FIRST,0,0,0,0);
        assert(cursor[lane].next == cursor[lane].end && cursor[lane].dropped);
        stream_trace_finish(t,cursor+lane,lane);
        assert(!cursor[lane].next && t->lane[lane].armed && t->lane[lane].dropped);
    }
    assert(t->lane[0].used == PRODUCER_EVENTS && t->lane[1].used == FILL_EVENTS);
    assert(time_reads == 2); // entire capture, not per event
    stream_trace_free(t); assert(!live_allocations);

    t = stream_trace_new(STREAM_TRACE_ALLOCATE,1000,950,54);
    cycle_now = UINT32_MAX-500;
    stream_trace_arm(t,cursor,0);
    stream_trace_event(cursor,TRACE_PUBLISH,STREAM_TRACE_FIRST,0,0,0,0); // CCOUNT crosses wrap
    stream_trace_event(cursor,TRACE_PUBLISH,STREAM_TRACE_FIRST,1,1,0,0);
    stream_trace_finish(t,cursor,0);
    printf("{\"test\":\"trace_clock\""); stream_trace_print(t); puts("}");
    assert(!t->lane[1].armed && !t->lane[1].used);
    // The fill can reach the first captured frame before the producer arms.
    // Pick the earlier armed anchor, including boot-microsecond wrap.
    t->lane[1].armed = 1;
    t->lane[1].clock.us = t->lane[0].clock.us-20;
    printf("{\"test\":\"early_fill\""); stream_trace_print(t); puts("}");
    stream_trace_free(t); assert(!live_allocations);
    puts("PSRAM-only/header-only allocation, two anchors, one timestamp/event, wrap, overflow and cleanup: ok");
    return 0;
}
