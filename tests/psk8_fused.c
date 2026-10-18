// Run the actual producer and bank mux with all three physical queue regions.
#define FUSED_MEMORY_TEST
#include "psk8_memory.c"

static uint32_t fused_run(unsigned mc, bool fixture, size_t available, int failure, bool fused)
{
    budget_active = false;
    size_t limit = queue_allocation_limit; queue_allocation_limit = 0;
    assert(start_encoder_options(fixture ? "dvbpace" : "dvb",mc,128,0,3,80000000,true,false,true));
    queue_allocation_limit = limit;
    budget_base = internal_used()+available; budget_min = available; budget_active = true;
    fail_after = failure;
    assert(radio_s3_stream_ready() == ESP_OK);
    fail_after = -1;
    assert(enc.fused == fused && dvb.queue_pages == 54);
    assert(budget_free(MALLOC_CAP_INTERNAL) >= LOFTY_HEAP_FLOOR+LOFTY_HEAP_GUARD);
    assert(budget_minimum(MALLOC_CAP_INTERNAL) >= LOFTY_HEAP_FLOOR);
    assert(radio_s3_stream_ready() == ESP_OK);
    if (fused && queue_allocation_limit) assert(enc.queue_extra);
    if (enc.queue_extra) assert(enc.queue_split > 30 && enc.queue_split < 54);
    expect_wifi = true; expect_pages = 54;
    radio_s3_stats_t stats;
    assert(pace(true,10,80000000,fill,NULL,&stats) == ESP_OK);
    uint32_t crc = pace_crc;
    printf("@{\"test\":\"fused\",\"mc\":%u,\"fixture\":%s,\"split\":%s,\"selected\":%s,\"crc\":%u",
           mc,fixture ? "true" : "false",enc.queue_extra ? "true" : "false",fused ? "true" : "false",crc);
    stop_encoder(); puts("}"); budget_active = false;
    assert(!live_allocations);
    return crc;
}

int main(void)
{
    callsign = "N0CALL";
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    for (unsigned mc=13; mc<=17; mc+=2) for (unsigned fixture=0; fixture<2; ++fixture) {
        uint32_t reference_crc = queue_budget(mc,fixture,true,32000,54,0);
        assert(fused_run(mc,fixture,40000,-1,true) == reference_crc);
        // With a tight budget or every optional allocation failing, retain
        // main's static 54 pages and exactly the same DAC stream.
        assert(fused_run(mc,fixture,32000,-1,false) == reference_crc);
        assert(fused_run(mc,fixture,40000,0,false) == reference_crc);
        // Cap each allocation to force two heap regions and a partial failure.
        queue_allocation_limit = 7000;
        assert(fused_run(mc,fixture,40000,-1,true) == reference_crc);
        assert(fused_run(mc,fixture,40000,1,false) == reference_crc);
        queue_allocation_limit = 0;
        setenv("FORCE_DUMMIES","1",1);
        reference_crc = queue_budget(mc,fixture,true,32000,54,0);
        assert(fused_run(mc,fixture,40000,-1,true) == reference_crc);
        unsetenv("FORCE_DUMMIES");
    }
    native_keyed_fixture = true; native_keyed_frames = 8;
    for (unsigned mc=13; mc<=17; mc+=2) for (unsigned solo=0; solo<2; ++solo)
    for (unsigned fused=0; fused<2; ++fused) {
        dvbtx_run_options(2412,mc,30,0,128,3,80000000,true,solo,fused);
        assert(enc.fused == fused && !live_allocations);
        expect_wifi = true; expect_pages = 54;
        dvbtx_pace_options(mc,10,128,0,3,80000000,solo,fused);
        assert(enc.fused == fused && !live_allocations);
    }
    native_keyed_fixture = false;
    free(banks);
    puts("fused queue: all DAC words match legacy CRCs, 54 pages, fallback and cleanup pass");
}
