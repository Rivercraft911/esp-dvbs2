// Run the real isolated command and callback under a host bank scheduler. No hardware.
#include "native.h"
#include "dvbs2.h"
#include "radio_s3.h"
#include "station_id.h"
static FILE *capture;
static unsigned calls, paced_calls;
static const char *station_store_get(void) { return "N0CALL"; }

// Ordinary malloc need not satisfy PIE's 16-byte workspace contract. The shared
// native allocator used to align every allocation and hid the probe's bad caller.
static void *unaligned[64];
static void *probe_malloc(size_t n, unsigned caps)
{
    uint8_t *base = heap_caps_malloc(n+8,caps);
    if (!base) return NULL;
    for (unsigned i = 0; i < 64; ++i) if (!unaligned[i]) {
        unaligned[i] = base+8;
        return base+8;
    }
    abort();
}
static void *allocation_base(void *p)
{
    for (unsigned i = 0; i < 64; ++i)
        if (p && unaligned[i] == p) return (uint8_t *)p-8;
    return p;
}
static void probe_free(void *p)
{
    heap_caps_free(allocation_base(p));
    for (unsigned i = 0; i < 64; ++i) if (unaligned[i] == p) unaligned[i] = NULL;
}
static size_t probe_size(void *p) { return heap_caps_get_allocated_size(allocation_base(p)); }
static int checked_bb(const dvbs2_fast *f, dvbs2_workspace *ws,
                       const uint8_t *payload, size_t bits)
{
    assert(!((uintptr_t)ws & 15));
    return dvbs2_fast_bb_gcs(f,ws,payload,bits);
}
#define heap_caps_malloc probe_malloc
#define heap_caps_free probe_free
#define heap_caps_get_allocated_size probe_size
#define dvbs2_fast_bb_gcs checked_bb
#include "../firmware/main/qpsk_probe.c"

static uint32_t banks[2][RADIO_S3_BANK_WORDS] __attribute__((aligned(16)));
static uint32_t oracle[16] __attribute__((aligned(16)));

int usb_serial_jtag_write_bytes(const void *p, size_t n, unsigned timeout)
{
    (void)timeout;
    assert(capture && fwrite(p,1,n,capture) == n);
    return n;
}

static void bank_fill(radio_s3_fill_fn fn, void *ctx, unsigned b, uint64_t first)
{
    const size_t offset = first % RADIO_S3_BANK_WORDS;
    const size_t head = RADIO_S3_BANK_WORDS-offset;
    assert(fn(banks[b]+offset,first,head,ctx) == ESP_OK);
    if (offset) assert(fn(banks[b],first+head,offset,ctx) == ESP_OK);
    probe_t *p = ctx;
    // Scalar oracle, including packed-source wrap and the initial history. Check every word.
    uint64_t last = UINT64_MAX;
    for (uint64_t at = first; at < first+RADIO_S3_BANK_WORDS; ++at) {
        const uint64_t block = at/(4*p->n);
        if (block != last) {
            uint8_t src[3];
            for (unsigned i = 0; i < 3; ++i)
                src[i] = block+i < 2 ? 0 : p->source[(block+i-2)%p->source_bytes];
            shaper_qpsk_c(oracle,src+2,1,shaper_store,p->n);
            last = block;
        }
        assert(banks[b][at%RADIO_S3_BANK_WORDS] == oracle[at%(4*p->n)]);
    }
}

esp_err_t radio_s3_dry_run_rate(radio_s3_fill_fn fn, void *ctx, uint64_t samples,
                                uint32_t rate, radio_s3_sink_fn sink_fn, void *sink_ctx,
                                radio_s3_dry_stats_t *stats)
{
    ++calls;
    assert(rate == 80000000);
    const uint64_t step = RADIO_S3_STEP_FOR(rate), lead = RADIO_S3_LEAD_FOR(rate);
    *stats = (radio_s3_dry_stats_t){0};
    bank_fill(fn,ctx,0,0);
    for (uint64_t k = 0; k*step < samples; ++k) {
        if (k) bank_fill(fn,ctx,k&1,k*step-lead);
        if (k) for (uint64_t at = k*step-lead; at < (k-1)*step+(k>1 ? 0 : lead)-lead+RADIO_S3_BANK_WORDS; ++at) {
            assert(banks[0][at%RADIO_S3_BANK_WORDS] == banks[1][at%RADIO_S3_BANK_WORDS]);
            ++stats->overlap_words;
        }
        const size_t count = samples-k*step < step ? samples-k*step : step;
        assert(sink_fn(banks[k&1],k*step,count,sink_ctx) == ESP_OK);
        ++stats->fills;
    }
    probe_t *p = ctx;
    assert(p->shaped+p->reused == (uint64_t)stats->fills*RADIO_S3_BANK_WORDS);
    return ESP_OK;
}

esp_err_t radio_s3_paced_run_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn, void *ctx,
                                  radio_s3_stats_t *stats)
{
    ++paced_calls;
    assert(sec == 1 && rate == 80000000);
    *stats = (radio_s3_stats_t){0};
    for (unsigned k = 0; k < 822; ++k)
        bank_fill(fn,ctx,k&1,k ? (uint64_t)k*RADIO_S3_STEP_WORDS_80M-RADIO_S3_LEAD_WORDS_80M : 0);
    probe_t *p = ctx;
    assert(p->shaped+p->reused == 822ull*RADIO_S3_BANK_WORDS);
    assert(p->reused == 821ull*640+128);
    // Fully cached, empty, and invalid backward requests exercise all callback exits.
    uint64_t shaped = p->shaped;
    assert(fn(banks[0],p->end-4,4,ctx) == ESP_OK && p->shaped == shaped);
    assert(fn(banks[0],p->end,0,ctx) == ESP_OK && p->shaped == shaped);
    assert(fn(banks[0],p->end-CACHE-1,1,ctx) == ESP_FAIL);
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    const unsigned n = strtoul(argv[1],NULL,10), mc = strtoul(argv[2],NULL,10);
    capture = fopen(argv[3],"wb"); assert(capture);
    char args[80];
    snprintf(args,sizeof args,"qpsk%u 16 %u",n,mc);
    qpsk_probe_command("dvbdump",args);
    assert(calls == 1 && live_allocations == 0);
    fclose(capture); capture = NULL;
    snprintf(args,sizeof args,"qpsk%u 1 %u",n,mc);
    qpsk_probe_command("dvbpace",args);
    assert(paced_calls == 1 && live_allocations == 0);
    // Every allocation failure must stop before starting the scheduler and leak nothing.
    for (int f = 0; f < 7; ++f) {
        fail_after = f;
        snprintf(args,sizeof args,"qpsk%u 16 %u",n,mc);
        qpsk_probe_command("dvbdump",args);
        assert(calls == 1 && live_allocations == 0);
    }
    fail_after = -1;
    const char *bad[] = {"", "qpsk", "qpsk3", "qpsk3 0", "qpsk5 1", "qpsk3 1 12",
        "qpsk3 1 -1", "qpsk3 -1", "qpsk3 1 11 extra", "qpsk3 61", "qpsk3 1x", "qpsk3 1 x"};
    for (unsigned i = 0; i < sizeof bad/sizeof *bad; ++i) {
        strcpy(args,bad[i]); qpsk_probe_command("dvbpace",args);
        assert(paced_calls == 1 && live_allocations == 0);
    }
    return 0;
}
