// Post-Wi-Fi queue admission and allocation cleanup. No host timing claims.
#include "native.h"
#include <stdbool.h>
// A finite, priority-ordered heap fixture, not a model of silicon addresses.
// The old order lets the context and workspace split the only cache-sized
// region. Match the IDF first-fitting-region policy and charge block overhead.
static bool fragmented;
static size_t queue_allocation_limit;
static const size_t region_size[] = {16000,31744,19000};
static size_t region_free[3], first_request;
static struct { void *p; unsigned region; size_t bytes; } region_blocks[64];
static void *fragment_alloc(size_t align, size_t n, unsigned caps)
{
    if ((caps & MALLOC_CAP_INTERNAL) && queue_allocation_limit && n > queue_allocation_limit) {
        ++allocation_count; return NULL;
    }
    if (!fragmented || !(caps & MALLOC_CAP_INTERNAL)) return heap_caps_aligned_alloc(align,n,caps);
    if (!first_request) first_request = n;
    for (unsigned r = 0; r < 3; ++r) {
        if (n+32 > region_free[r]) continue;
        void *p = heap_caps_aligned_alloc(align,n,caps);
        if (p) for (unsigned i = 0; i < 64; ++i) if (!region_blocks[i].p) {
            region_blocks[i].p = p; region_blocks[i].region = r; region_blocks[i].bytes = n+32;
            region_free[r] -= n+32;
            return p;
        }
        assert(!p);
        return NULL;
    }
    ++allocation_count;
    return NULL;
}
static void fragment_free(void *p)
{
    for (unsigned i = 0; i < 64; ++i) if (p && region_blocks[i].p == p) {
        region_free[region_blocks[i].region] += region_blocks[i].bytes;
        region_blocks[i].p = NULL;
    }
    heap_caps_free(p);
}
#define heap_caps_aligned_alloc fragment_alloc
#define heap_caps_free fragment_free
static bool budget_active;
static size_t budget_base, budget_min;
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
#define heap_caps_get_free_size budget_free
#undef heap_caps_get_minimum_free_size
#define heap_caps_get_minimum_free_size budget_minimum
static void counted_samples(uint32_t *, const uint32_t *, unsigned, size_t, const uint32_t *);
#define lofty_samples counted_samples
#define NATIVE_PACE_TEST
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#undef lofty_samples
void lofty_samples(uint32_t *, const uint32_t *, unsigned, size_t, const uint32_t *);
static unsigned warm_words;

static void counted_samples(uint32_t *dst, const uint32_t *src, unsigned skip, size_t n, const uint32_t *table)
{
    if ((uintptr_t)dst >= (uintptr_t)dvb.samples &&
        (uintptr_t)dst < (uintptr_t)(dvb.samples+dvb.cached_words)) {
        assert(!dvb.sample_end && !lofty_mux.output && !dvb.fill_calls);
        assert(dst+n <= dvb.samples+dvb.cached_words);
        warm_words += n;
    }
    lofty_samples(dst,src,skip,n,table);
}

static bool expect_wifi;
static unsigned expect_pages, pace_calls;
static uint32_t pace_crc;
static esp_err_t pace(bool, uint32_t, uint32_t, radio_s3_fill_fn, void *, radio_s3_stats_t *);

static uint32_t queue_budget(unsigned mc, bool fixture, bool automatic, size_t available,
                              unsigned expected, unsigned failure)
{
    budget_active = false;
    warm_words = 0;
    const char *cmd = fixture ? "dvbpace" : "dvb";
    assert(start_encoder_options(cmd,mc,128,0,3,80000000,automatic,false));
    if (fixture) assert(enc.input && esp_ptr_external_ram(enc.input));
    else assert(!enc.input);
    if (automatic) {
        assert(!enc.block && !dvb.queue_pages && dvb.produced == dvb.consumed && !enc.frames);
        budget_base = internal_used()+available;
        budget_min = failure == 1 ? 19999 : available;
        budget_active = true;
        const int allocations = allocation_count;
        if (failure == 2) fail_after = 0;
        const esp_err_t rc = radio_s3_stream_ready();
        fail_after = -1;
        assert(allocation_count == allocations); // no allocation after Wi-Fi
        if (expected) {
            assert(rc == ESP_OK && lofty_mux.queue_ready && dvb.queue_pages == expected);
            assert(dvb.produced-dvb.consumed == expected && !lofty_mux.output && !lofty_mux.frames && !dvb.sample_end);
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
    assert(warm_words == (automatic && expected ? 39983u : 0u));
    printf("@{\"test\":\"%s\",\"modcod\":%u,\"setup\":\"%s\",\"expected_pages\":%u,\"crc\":%u,\"renderer_warm_words\":%u",
           fragmented ? "queue_fragmentation" : "queue_budget",mc,cmd,expected,crc,warm_words);
    stop_encoder(); puts("}");
    budget_active = false;
    assert(!live_allocations);
    return crc;
}

static void fragmented_order(unsigned mc)
{
    assert(!live_allocations);
    fragmented = true;
    memcpy(region_free,region_size,sizeof region_free);
    first_request = 0;
    const unsigned caps = MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    const dvbs2_mode *mode = dvbs2_find_mode(mc);
    dvbs2_fast *fast = heap_caps_aligned_alloc(16,dvbs2_fast_mode_size(mode),caps);
    void *ws = heap_caps_aligned_alloc(16,DVBS2_FAST_WORKSPACE_USED,caps);
    assert(fast && ws && !dvbs2_fast_prepare(fast,mode,1,15));
    const size_t bytes = dvbs2_fast_cache_size(fast)+DVBS2_LOFTY_CACHE_BYTES;
    const size_t tail = dvbs2_fast_cache_tail_size(fast);
    void *cache = heap_caps_aligned_alloc(16,bytes-tail,caps);
    assert(!!cache == (mc == 13)); // old order reproduces the mode-dependent failure
    heap_caps_free(cache); heap_caps_free(ws); heap_caps_free(fast);
    assert(!live_allocations && !memcmp(region_free,region_size,sizeof region_free));
    for (unsigned fixture = 0; fixture < 2; ++fixture) {
        first_request = 0;
        queue_budget(mc,fixture,true,32000,54,0); // actual producer, fill and teardown
        assert(first_request == bytes-tail); // resident prefix precedes context/workspace
        assert(!memcmp(region_free,region_size,sizeof region_free));
    }
    fragmented = false;
}

static esp_err_t pace(bool wifi, uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                       void *ctx, radio_s3_stats_t *stats)
{
    assert(wifi == expect_wifi && sec == 10 && rate == 80000000 && fn == fill);
    if (wifi) assert(radio_s3_stream_ready() == ESP_OK);
    assert(dvb.queue_pages == expect_pages);
    if (dvb.lofty) {
        assert(enc.table_tail && esp_ptr_external_ram(enc.table_tail));
        assert(!enc.ws_borrowed && heap_caps_get_allocated_size(enc.ws) == DVBS2_FAST_WORKSPACE_USED);
        assert(!enc.block);
        for (unsigned i = 1; i < expect_pages && i < LOFTY_STORED; ++i)
            assert(lofty_page_at(i) == lofty_page_at(i-1)+1);
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
    if (dvb.lofty && !getenv("FORCE_DUMMIES")) assert(lofty_mux.frames == 8 && !lofty_mux.startup && !lofty_mux.steady);
    if (dvb.lofty) {
        assert(!dvb.copy.calls && !dvb.whole.calls && !dvb.save.calls && !dvb.notify.calls);
        assert(dvb.fill_calls);
        if (dvb.profile) assert(enc.stage[0].calls && enc.stage[4].calls);
    } else assert(dvb.copy.calls && dvb.whole.calls);
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
    // Public paces choose Wi-Fi and gate the queue after its allocations.
    expect_wifi = true; expect_pages = 54;
    dvbtx_pace(17,10,128,0,3,80000000);
    assert(!live_allocations);
    expect_pages = MACHINE_PAGES;
    dvbtx_pace(11,10,128,0,DVB_SPS_12_5,80000000);
    assert(!live_allocations && pace_calls == 2);
    // Fail each setup allocation, including the dummy and PSRAM payload.
    allocation_count = 0;
    assert(start_encoder_options("dvbpace",17,128,0,3,80000000,true,false));
    const int allocations = allocation_count;
    stop_encoder(); puts("}");
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder_options("dvbpace",17,128,0,3,80000000,true,false));
        assert(!live_allocations);
    }
    fail_after = -1;
    for (unsigned mc = 13; mc <= 17; mc += 2)
        for (unsigned fixture = 0; fixture < 2; ++fixture) {
            const unsigned pages = mc == 13 ? 54 : mc == 15 ? 50 : 46;
            const uint32_t fixed = queue_budget(mc,fixture,false,0,pages,0);
            assert(queue_budget(mc,fixture,true,32000,54,0) == fixed);
        }
    const uint32_t fixed = queue_budget(17,true,false,0,46,0);
    assert(queue_budget(17,true,true,26500,54,0) == fixed);
    assert(queue_budget(17,true,true,22048,54,0) == fixed); // exact reserve
    queue_budget(17,true,true,22047,0,0); // below reserve
    queue_budget(17,true,true,32000,0,1); // startup low-water mark already below floor
    assert(queue_budget(17,true,true,32000,54,2) == fixed); // allocation disabled after Wi-Fi
    for (unsigned mc = 13; mc <= 17; mc += 2) fragmented_order(mc);
    free(banks);
    puts("queue admission: identical words, allocation cleanup, Wi-Fi routing and defaults: ok");
    return 0;
}
