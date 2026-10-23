// Queue admission without heap use, and run-memory cleanup. No host timing claims.
#include "native.h"
#include <stdbool.h>
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

static bool expect_ready;
static unsigned expect_pages, pace_calls;
static uint32_t pace_crc;
static esp_err_t pace(bool, uint32_t, uint32_t, radio_s3_fill_fn, void *, radio_s3_stats_t *);

static uint32_t queue_check(unsigned mc, bool fixture, bool automatic, unsigned expected)
{
    warm_words = 0;
    const char *cmd = fixture ? "dvbpace" : "dvb";
    assert(start_encoder_options(cmd,mc,128,0,3,80000000,automatic,fixture));
    if (fixture) assert(enc.input && esp_ptr_external_ram(enc.input));
    else assert(!enc.input);
    if (automatic) {
        assert(!enc.block && !dvb.queue_pages && dvb.produced == dvb.consumed && !enc.frames);
        const int allocations = allocation_count;
        assert(radio_s3_stream_ready() == ESP_OK);
        assert(allocation_count == allocations); // the gate takes no memory
        assert(lofty_mux.queue_ready && dvb.queue_pages == expected);
        assert(dvb.produced-dvb.consumed == expected && !lofty_mux.output && !lofty_mux.frames && !dvb.sample_end);
        assert(radio_s3_stream_ready() == ESP_OK); // no second prefill
    }
    expect_ready = automatic; expect_pages = expected;
    radio_s3_stats_t stats;
    assert(pace(automatic,10,80000000,fill,NULL,&stats) == ESP_OK);
    const uint32_t crc = pace_crc;
    assert(warm_words == (automatic ? 39983u : 0u));
    printf("@{\"test\":\"queue_check\",\"modcod\":%u,\"setup\":\"%s\",\"expected_pages\":%u,\"crc\":%u,\"renderer_warm_words\":%u",
           mc,cmd,expected,crc,warm_words);
    stop_encoder(); puts("}");
    assert(run_returned());
    return crc;
}

static esp_err_t pace(bool ready, uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                       void *ctx, radio_s3_stats_t *stats)
{
    assert(ready == expect_ready && sec == 10 && rate == 80000000 && fn == fill);
    if (ready) assert(radio_s3_stream_ready() == ESP_OK);
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
    // Paces time whole fills only: per-phase marks belong to dumps (dvb_fill.inc).
    assert(!dvb.copy.calls && !dvb.whole.calls && !dvb.save.calls && !dvb.notify.calls);
    assert(dvb.fill_calls);
    if (dvb.lofty && dvb.profile) assert(enc.stage[0].calls && enc.stage[4].calls);
    memset(stats,0,sizeof *stats);
    return ESP_OK;
}

esp_err_t radio_s3_paced_run(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn, void *ctx,
                             const radio_s3_options_t *options, radio_s3_stats_t *stats)
{ (void)options; return pace(true,sec,rate,fn,ctx,stats); }

int main(void)
{
    callsign = "N0CALL";
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    // Public paces gate the queue in radio_s3_stream_ready, as the radio worker does.
    expect_ready = true; expect_pages = 54;
    dvbtx_pace(17,10,128,0,3,80000000,true);
    assert(run_returned());
    expect_pages = MACHINE_PAGES;
    dvbtx_pace(11,10,128,0,DVB_SPS_12_5,80000000,true);
    assert(run_returned() && pace_calls == 2);
    // Fail each setup allocation, including the dummy and PSRAM payload.
    allocation_count = 0;
    assert(start_encoder_options("dvbpace",17,128,0,3,80000000,true,false));
    const int allocations = allocation_count;
    stop_encoder(); puts("}");
    for (int i = 0; i < allocations; ++i) {
        fail_after = i;
        assert(!start_encoder_options("dvbpace",17,128,0,3,80000000,true,false));
        assert(run_returned());
    }
    fail_after = -1;
    for (unsigned mc = 13; mc <= 17; mc += 2)
        for (unsigned fixture = 0; fixture < 2; ++fixture) {
            const unsigned pages = mc == 13 ? 54 : mc == 15 ? 50 : 46;
            const uint32_t fixed = queue_check(mc,fixture,false,pages);
            assert(queue_check(mc,fixture,true,54) == fixed);
        }
    free(banks);
    puts("queue admission: identical words, no memory taken at the gate, allocation cleanup and defaults: ok");
    return 0;
}
