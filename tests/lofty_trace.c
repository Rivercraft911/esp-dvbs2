// Real concurrent producer/consumer. Host pacing is functional, not a timing model.
#include "native.h"
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main

static uint32_t capture_trace(unsigned mc, bool trace)
{
    callsign = "N0CALL";
    assert(start_encoder_traced("dvbpace",mc,128,0,3,80000000,trace));
    uint32_t *buf = NULL;
    assert(!posix_memalign((void **)&buf,16,5*MACHINE_PAGE_SAMPLES*4));
    const uint64_t stop = 130ull*3*LOFTY_FRAME_SYMBOLS+6ull*DVBS2_DUMMY_SYMBOLS;
    uint32_t crc = 0;
    while (lofty_mux.output < stop) {
        size_t n = stop-lofty_mux.output;
        if (n > 5*MACHINE_PAGE_SAMPLES) n = 5*MACHINE_PAGE_SAMPLES;
        assert(!lofty_shape(buf,lofty_mux.output,n));
        crc = esp_rom_crc32_le(crc,(uint8_t *)buf,4*n);
        if (dvb.wake_pending) { moon_wake(); dvb.wake_pending = false; }
        ++dvb.fill_calls;
    }
    assert(lofty_mux.frames == 130 && !dvb.queue_empty);
    assert(lofty_mux.steady == 2 && !lofty_mux.startup);
    printf("@{\"test\":\"trace_lifecycle\",\"modcod\":%u,\"traced\":%s,\"crc\":%u",
           mc,trace ? "true" : "false",crc);
    stop_encoder(); puts("}");
    assert(!live_allocations);
    free(buf);
    return crc;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    unsigned mc = atoi(argv[1]);
    assert(capture_trace(mc,false) == capture_trace(mc,true));
    return 0;
}
