// Window edges, shared clock wrap, dummy accounting, capacity and allocation failure.
#include "native.h"
static int64_t now;
#define esp_timer_get_time trace_time
static int64_t trace_time(void) { return now; }
#include "../firmware/main/stream_trace.c"
#undef esp_timer_get_time

int main(void)
{
    fail_after = 0; assert(!stream_trace_new()); fail_after = -1;
    now = UINT32_MAX-5;
    stream_trace_t *t = stream_trace_new();
    assert(t && live_allocations == 1);
    now += 12;
    stream_trace_event(t,0,TRACE_FEC_START,63,0,0,0,0);
    stream_trace_event(t,0,TRACE_FEC_START,128,0,0,0,0);
    assert(!t->lane[0].used);
    stream_trace_event(t,0,TRACE_FEC_START,64,UINT32_MAX,UINT32_MAX-1,0,0);
    assert(t->producer[0].us == 12);
    for (unsigned i = 0; i < 2; ++i)
        stream_trace_event(t,1,TRACE_ADMIT,64,1,0,1<<19,0);
    stream_trace_event(t,1,TRACE_ADMIT,64,2,0,0,0);
    assert(t->fill[2].b == 2 && !t->lane[1].dummies);
    stream_trace_event(t,1,TRACE_ADMIT,65,2,1,0,0);
    assert(!t->fill[3].b);
    stream_trace_event(t,0,TRACE_BOUND,1,0,0,13268,41 | (43<<8));
    stream_trace_event(t,0,TRACE_GAP_MAX,0,0,0,13268,1536);
    assert(t->lane[0].used == 3);
    for (unsigned lane = 0; lane < 2; ++lane)
        for (unsigned i = 0; i < PRODUCER_EVENTS+1; ++i)
            stream_trace_event(t,lane,TRACE_PUBLISH,64,0,0,0,0);
    assert(t->lane[0].used == PRODUCER_EVENTS && t->lane[1].used == FILL_EVENTS);
    assert(t->lane[0].dropped && t->lane[1].dropped);
    stream_trace_free(t); assert(!live_allocations);
    puts("trace window, clock wrap, dummies, full lanes and allocation cleanup: ok");
    return 0;
}
