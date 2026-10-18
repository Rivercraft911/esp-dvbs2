// Real concurrent producer/consumer. Host pacing is functional, not a timing model.
#include "native.h"
#include "stream_trace.h"
static unsigned event_calls, trace_allocations;
static stream_trace_t *checked_trace_new(uint32_t, uint32_t, uint32_t, unsigned);
#define stream_trace_new checked_trace_new
static void checked_event(stream_trace_lane_t *, unsigned, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define stream_trace_event checked_event
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#undef stream_trace_event
#undef stream_trace_new

static stream_trace_t *checked_trace_new(uint32_t frame, uint32_t put, uint32_t get, unsigned pages)
{
    assert(frame == STREAM_TRACE_ALLOCATE && enc.frames == frame);
    assert(put-get <= pages && frame-(put-get+43)/44 >= 64);
    ++trace_allocations;
    if (getenv("FAIL_TRACE_ALLOCATION")) return NULL;
    return stream_trace_new(frame,put,get,pages);
}

static uint32_t startup_layout(void)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 64; ++i) if (native_blocks[i].p) {
        h = (h^(uint32_t)native_blocks[i].n)*16777619u;
        h = (h^native_blocks[i].caps)*16777619u;
    }
    return h;
}

static void checked_event(stream_trace_lane_t *cursor, unsigned kind, uint32_t frame,
                          uint32_t put, uint32_t get, uint32_t a, uint32_t b)
{
    // Check the caller, rather than relying on a filter inside the recorder.
    assert(cursor->next && frame >= STREAM_TRACE_FIRST && frame < STREAM_TRACE_FIRST+STREAM_TRACE_FRAMES);
    __atomic_fetch_add(&event_calls,1,__ATOMIC_RELAXED);
    stream_trace_event(cursor,kind,frame,put,get,a,b);
}

static uint32_t capture_trace(unsigned mc, bool trace)
{
    callsign = "N0CALL";
    event_calls = trace_allocations = 0;
    allocation_count = 0;
    assert(start_encoder_traced("dvbpace",mc,128,0,3,80000000,trace));
    assert(!__atomic_load_n(&event_calls,__ATOMIC_RELAXED) && !dvb.trace_lane[0].next && !dvb.trace_lane[1].next);
    assert(!trace_allocations && !dvb.trace);
    const uint32_t layout = startup_layout();
    const unsigned start_allocations = allocation_count, prefill = enc.prefill_frames;
    uint32_t *buf = NULL;
    assert(!posix_memalign((void **)&buf,16,5*MACHINE_PAGE_SAMPLES*4));
    const unsigned frames = getenv("STARTUP_ONLY") ? 2 : STREAM_TRACE_FIRST+STREAM_TRACE_FRAMES+2;
    const uint64_t stop = (uint64_t)frames*3*LOFTY_FRAME_SYMBOLS+(frames != 2 ? 6ull*DVBS2_DUMMY_SYMBOLS : 0);
    uint32_t crc = 0;
    while (lofty_mux.output < stop) {
        size_t n = stop-lofty_mux.output;
        if (n > 5*MACHINE_PAGE_SAMPLES) n = 5*MACHINE_PAGE_SAMPLES;
        assert(!lofty_shape(buf,lofty_mux.output,n));
        if (lofty_mux.frames < 64) {
            assert(!__atomic_load_n(&event_calls,__ATOMIC_RELAXED));
            assert(startup_layout() == layout && (unsigned)allocation_count == start_allocations);
        }
        crc = esp_rom_crc32_le(crc,(uint8_t *)buf,4*n);
        if (dvb.wake_pending) { moon_wake(); dvb.wake_pending = false; }
        ++dvb.fill_calls;
    }
    assert(lofty_mux.frames == frames && !dvb.queue_empty);
    assert(!dvb.trace_lane[0].next && !dvb.trace_lane[1].next);
    if (trace && frames != 2 && !getenv("FAIL_TRACE_ALLOCATION")) assert(__atomic_load_n(&event_calls,__ATOMIC_RELAXED) > 5000);
    else assert(!__atomic_load_n(&event_calls,__ATOMIC_RELAXED));
    assert(lofty_mux.steady == (frames != 2 ? 2u : 0u) && !lofty_mux.startup);
    assert(trace_allocations == (unsigned)(trace && frames != 2));
    printf("@{\"test\":\"trace_lifecycle\",\"modcod\":%u,\"traced\":%s,\"crc\":%u,"
           "\"startup_layout\":%u,\"startup_allocations\":%u,\"startup_prefill\":%u",
           mc,trace ? "true" : "false",crc,layout,start_allocations,prefill);
    stop_encoder(); puts("}");
    assert(!live_allocations);
    free(buf);
    return crc;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    unsigned mc = atoi(argv[1]);
    dvbtx_pace(mc,10,128,0,3,80000000,0,true,true); // rejected before allocation/workers
    assert(!live_allocations);
    assert(capture_trace(mc,false) == capture_trace(mc,true));
    return 0;
}
