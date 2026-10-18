// Runtime queue geometry through the actual pace API. No host timing claims.
#include "native.h"
#include <stdbool.h>
static bool budget_active;
static size_t budget_base, budget_min, budget_largest;
static size_t internal_used(void)
{
    size_t n = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (native_blocks[i].p && (native_blocks[i].caps & MALLOC_CAP_INTERNAL))
            n += native_blocks[i].n+32; // model allocator overhead too
    return n;
}
static size_t budget_free(unsigned caps)
{
    if (!budget_active || !(caps & MALLOC_CAP_INTERNAL)) return 65536;
    const size_t used = internal_used();
    assert(used <= budget_base);
    const size_t n = budget_base-used;
    if (n < budget_min) budget_min = n;
    return n;
}
static size_t budget_minimum(unsigned caps)
{
    budget_free(caps);
    return budget_active && (caps & MALLOC_CAP_INTERNAL) ? budget_min : 65536;
}
static void *budget_alloc(size_t align, size_t n, unsigned caps)
{
    if (budget_active && (caps & MALLOC_CAP_INTERNAL) &&
        (n > budget_largest || n+32 > budget_free(caps))) return NULL;
    return heap_caps_aligned_alloc(align,n,caps);
}
#define heap_caps_get_free_size budget_free
#undef heap_caps_get_minimum_free_size
#define heap_caps_get_minimum_free_size budget_minimum
#define heap_caps_aligned_alloc budget_alloc
#define NATIVE_PACE_TEST
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main

static bool expect_wifi;
static unsigned expect_pages, pace_calls;
static uint32_t pace_crc;
static esp_err_t pace(bool, uint32_t, uint32_t, radio_s3_fill_fn, void *, radio_s3_stats_t *);

static uint32_t queue_budget(unsigned mc, bool fixture, bool automatic, size_t available,
                              size_t largest, unsigned expected, unsigned failure)
{
    budget_active = false;
    const char *cmd = fixture ? "dvbpace" : "dvb";
    assert(start_encoder_traced(cmd,mc,128,0,3,80000000,false,automatic ? 0 : 54,automatic));
    if (fixture) assert(enc.input && esp_ptr_external_ram(enc.input));
    else assert(!enc.input);
    if (automatic) {
        assert(!enc.block && !dvb.queue_pages && dvb.produced == dvb.consumed && !enc.frames);
        budget_base = internal_used()+available;
        budget_min = failure == 1 ? 19999 : available;
        budget_largest = largest;
        budget_active = true;
        if (failure == 2) fail_after = 0;
        const esp_err_t rc = radio_s3_stream_ready();
        fail_after = -1;
        if (expected) {
            assert(rc == ESP_OK && lofty_mux.queue_ready && dvb.queue_pages == expected);
            assert(budget_free(MALLOC_CAP_INTERNAL) >= LOFTY_HEAP_FLOOR+LOFTY_HEAP_GUARD);
            assert(budget_minimum(MALLOC_CAP_INTERNAL) >= LOFTY_HEAP_FLOOR);
            assert(radio_s3_stream_ready() == ESP_OK); // no second allocation/prefill
        } else {
            assert(rc == ESP_ERR_NO_MEM && !enc.block && !lofty_mux.queue_ready);
            assert(dvb.produced == dvb.consumed && !enc.frames);
        }
    }
    uint32_t crc = 0;
    if (expected) {
        expect_wifi = automatic; expect_pages = expected;
        radio_s3_stats_t stats;
        assert(pace(automatic,10,80000000,fill,NULL,&stats) == ESP_OK);
        crc = pace_crc;
    }
    printf("@{\"test\":\"queue_budget\",\"modcod\":%u,\"setup\":\"%s\",\"expected_pages\":%u,\"crc\":%u",
           mc,cmd,expected,crc);
    stop_encoder(); puts("}");
    budget_active = false;
    assert(!live_allocations);
    return crc;
}

static esp_err_t pace(bool wifi, uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                       void *ctx, radio_s3_stats_t *stats)
{
    assert(wifi == expect_wifi && sec == 10 && rate == 80000000 && fn == fill);
    if (wifi) assert(radio_s3_stream_ready() == ESP_OK);
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
    expect_wifi = true; expect_pages = 54;
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
    assert(start_encoder_traced("dvbpace",17,128,0,3,80000000,false,58,false));
    const int allocations = allocation_count;
    stop_encoder(); puts("}");
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder_traced("dvbpace",17,128,0,3,80000000,false,58,false));
        assert(!live_allocations);
    }
    fail_after = -1;
    for (unsigned mc = 13; mc <= 17; mc += 2)
        for (unsigned fixture = 0; fixture < 2; ++fixture) {
            const uint32_t fixed = queue_budget(mc,fixture,false,0,0,54,0);
            assert(queue_budget(mc,fixture,true,32000,SIZE_MAX,54,0) == fixed);
        }
    const uint32_t fixed = queue_budget(17,true,false,0,0,54,0);
    assert(queue_budget(17,true,true,26500,SIZE_MAX,46,0) == fixed);
    assert(queue_budget(17,true,true,32000,12*sizeof(lofty_page),50,0) == fixed);
    queue_budget(17,true,true,23000,SIZE_MAX,0,0); // cannot fit even 44 pages above reserve
    queue_budget(17,true,true,32000,SIZE_MAX,0,1); // startup low-water mark already below floor
    queue_budget(17,true,true,32000,SIZE_MAX,0,2); // every candidate allocation fails
    free(banks);
    puts("runtime pages: wraps, identical words, allocation cleanup, Wi-Fi routing and defaults: ok");
    return 0;
}
