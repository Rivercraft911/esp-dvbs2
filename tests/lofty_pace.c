// Runtime queue geometry through the actual pace API. No host timing claims.
#define NATIVE_PACE_TEST
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main

static bool expect_wifi;
static unsigned expect_pages, pace_calls;
static uint32_t pace_crc;

static esp_err_t pace(bool wifi, uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                       void *ctx, radio_s3_stats_t *stats)
{
    assert(wifi == expect_wifi && sec == 10 && rate == 80000000 && fn == fill);
    assert(dvb.queue_pages == expect_pages && !dvb.trace && !dvb.trace_state);
    assert(dvb.page_wake == !dvb.lofty);
    if (dvb.lofty) {
        assert(heap_caps_get_allocated_size(enc.block) == (expect_pages-LOFTY_STORED)*sizeof(lofty_page));
        assert(enc.prefill_frames == 1);
    }
    ++pace_calls;
    const uint64_t stop = 8ull*dvb.ring.frame_symbols*dvb.table.n/dvb.den;
    pace_crc = 0;
    for (uint64_t first = 0; first < stop;) {
        unsigned n = stop-first > 6144 ? 6144 : (unsigned)(stop-first);
        assert(fn(banks[0],first,n,ctx) == ESP_OK);
        pace_crc = esp_rom_crc32_le(pace_crc,(uint8_t *)banks[0],4*n);
        first += n;
    }
    assert(!dvb.queue_empty && !dvb.ring.underruns);
    if (dvb.lofty) assert(lofty_mux.frames == 8 && !lofty_mux.startup && !lofty_mux.steady);
    memset(stats,0,sizeof *stats);
    return ESP_OK;
}

esp_err_t radio_s3_paced_run_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                                  void *ctx, radio_s3_stats_t *stats)
{ return pace(false,sec,rate,fn,ctx,stats); }
esp_err_t radio_s3_paced_wifi_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                                   void *ctx, radio_s3_stats_t *stats)
{ return pace(true,sec,rate,fn,ctx,stats); }

int main(void)
{
    callsign = "N0CALL";
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    uint32_t golden = 0;
    const unsigned counts[] = {0,46,50,54,58,44,64};
    for (unsigned i = 0; i < sizeof counts/sizeof *counts; ++i) {
        expect_pages = counts[i] ? counts[i] : 46;
        expect_wifi = false;
        dvbtx_pace(17,10,128,0,3,80000000,0,false,false,counts[i],true);
        assert(!live_allocations);
        if (!i) golden = pace_crc;
        else assert(pace_crc == golden);
        printf("@{\"test\":\"pace_pages\",\"pages\":%u,\"crc\":%u}\n",expect_pages,pace_crc);
    }
    // Default 17 and qualified QPSK still select the Wi-Fi-up implementation.
    expect_wifi = true; expect_pages = 46;
    dvbtx_pace(17,10,128,0,3,80000000,0,false,false,0,false);
    assert(pace_crc == golden && !live_allocations);
    expect_pages = MACHINE_PAGES;
    dvbtx_pace(11,10,128,0,DVB_SPS_12_5,80000000,0,false,false,0,false);
    assert(!live_allocations && pace_calls == 9);
    // Invalid overrides and observer without Wi-Fi fail before allocation.
    allocation_count = 0;
    dvbtx_pace(17,10,128,0,3,80000000,0,false,false,43,true);
    dvbtx_pace(17,10,128,0,3,80000000,0,false,false,65,true);
    dvbtx_pace(11,10,128,0,DVB_SPS_12_5,80000000,0,false,false,46,true);
    dvbtx_pace(17,10,128,0,3,80000000,0,true,false,46,true);
    assert(!allocation_count && !live_allocations && pace_calls == 9);
    // The larger queue owns no memory on any allocation-failure return.
    allocation_count = 0;
    assert(start_encoder_traced("dvbpace",17,128,0,3,80000000,false,58));
    const int allocations = allocation_count;
    stop_encoder(); puts("}");
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder_traced("dvbpace",17,128,0,3,80000000,false,58));
        assert(!live_allocations);
    }
    free(banks);
    puts("runtime pages: wraps, identical words, allocation cleanup, Wi-Fi routing and defaults: ok");
    return 0;
}
