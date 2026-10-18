// Event-clock producer and real page consumer. No scheduler or board clock.
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main

static unsigned frame, page, fec, bulk;
static uint64_t next;
static bool blocked, slow;

static void publish_page(void)
{
    lofty_page *p = lofty_page_at(dvb.put_index);
    memset(p,0,sizeof *p);
    for (unsigned i = 0; i < LOFTY_BLOCKS; ++i) p->words[i] = lofty_descriptor(0);
    unsigned prefix = frame&1 ? 6 : 0;
    lofty_mux.skip = page ? 0 : prefix;
    dvb.mapper.at = page == 43 ? LOFTY_FRAME_SYMBOLS : (page+1)*512-prefix/3;
    lofty_publish(page == 43 ? 534+prefix : MACHINE_PAGE_SAMPLES);
    if (++page == 44) { page = 0; ++frame; ++lofty_mux.published; }
}

static void advance(uint64_t time)
{
    while (!blocked && next <= time) {
        if (dvb.produced-dvb.consumed == dvb.queue_pages) { blocked = true; break; }
        publish_page();
        unsigned cost = page == 0 ? fec : page == 1 ? bulk : 1000;
        if (slow && page == 0 && frame%19 == 0) cost += 180000;
        next += cost;
        if (page && dvb.produced-dvb.consumed == dvb.queue_pages) blocked = true;
    }
    dvb.producer_phase = blocked ? QUEUE_WAIT : page ? QUEUE_MAP : QUEUE_FEC;
}

static void envelope_one(bool fused)
{
    enc.fused = fused;
    const unsigned period = (fused ? 21 : 23)*128;
    for (unsigned wake = 0; wake <= 47220; wake += 47220)
        for (unsigned gap = 0; gap <= 100000; gap += 17) {
            unsigned brute = 44;
            for (unsigned q = 2; q < 44; ++q) {
                bool safe = true;
                for (unsigned k = 1; k <= 44-q; ++k) {
                    int64_t deadline = ((int64_t)q-9+k-1)*period;
                    if ((int64_t)wake+2*(int64_t)gap*k > deadline) safe = false;
                }
                if (safe) { brute = q; break; }
            }
            assert((fused ? lofty_admission_fused(gap,wake) : lofty_admission(gap,wake)) == brute);
        }
    assert((fused ? lofty_admission_fused(1000,0) : lofty_admission(1000,0)) == 10);
    assert((fused ? lofty_admission_fused(1000,47220) : lofty_admission(1000,47220)) == (fused ? 28 : 26));
    for (unsigned wake = 0; wake <= 47220; wake += 47220) {
        unsigned limit = (34*period-wake)/2;
        assert((fused ? lofty_admission_fused(limit,wake) : lofty_admission(limit,wake)) == 43);
        assert((fused ? lofty_admission_fused(limit+1,wake) : lofty_admission(limit+1,wake)) == 44);
        assert((fused ? lofty_admission_fused(UINT32_MAX,wake) : lofty_admission(UINT32_MAX,wake)) == 44);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    unsigned mc = atoi(argv[1]); slow = atoi(argv[2]);
    envelope_one(false); envelope_one(true); enc.fused = false;
    setenv("UNPACED_LOFTY","1",1);
    callsign = "N0CALL";
    assert(start_encoder("dvbpace",mc,128,0,3,80000000));
    // Retain real allocations, stop only the encoder. The event model owns
    // publication; the production consumer still owns every page release.
    enc.stop = true; xTaskNotifyGive(enc.task);
    xSemaphoreTake(enc.exited,portMAX_DELAY);
    vTaskDelete(enc.task); enc.task = NULL;
    // EOF may be visible before the completed-frame counter.
    dvb.produced = dvb.consumed; dvb.waiting = 0;
    lofty_mux.frames = 7; lofty_boundary(6); assert(lofty_mux.filler);
    lofty_mux.startup = lofty_mux.steady = 0;
    lofty_mux.frames = UINT32_MAX; dvb.produced = dvb.consumed+44;
    lofty_boundary(0); assert(!lofty_mux.filler);
    dvb.produced = dvb.consumed = UINT32_MAX-15;
    dvb.put_index = dvb.get_index = dvb.page_sample = 0;
    dvb.queue_sample = 0; dvb.prepare_pages = 0;
    dvb.page_clock = dvb.page_wait = 0;
    lofty_mux.output = lofty_mux.samples = lofty_mux.published = lofty_mux.frames = 0;
    lofty_mux.length = lofty_mux.startup = lofty_mux.steady = lofty_mux.streaming = 0;
    lofty_mux.history = lofty_mux.skip = lofty_mux.head_left = 0;
    lofty_mux.splice = false;
    lofty_mux.page_max = 1000;
    lofty_mux.admission_pages = lofty_admission(1000,0) | (lofty_admission(1000,47220)<<8);
    // Tail-page timing is an assumed service bound. Other costs sum to the
    // round-10 means; these are model inputs, not new timing measurements.
    unsigned mean = mc == 13 ? 168152 : mc == 15 ? 174332 : 153882;
    bulk = mc == 13 ? 43595 : mc == 15 ? 44262 : 44762;
    fec = mean-bulk-42000;
    // Test both minimum-prefix admission and the firmware's full prefill.
    unsigned prefill = argc == 4 ? dvb.queue_pages : 10;
    for (unsigned i = 0; i < prefill; ++i) publish_page();
    blocked = prefill == dvb.queue_pages;
    next = 1000;
    uint32_t *dst; assert(!posix_memalign((void **)&dst,16,1540*sizeof *dst));
    for (uint64_t t = 0; lofty_mux.frames < 1000; t += 47220) {
        assert(t < UINT64_C(400000000));
        uint64_t now = t;
        unsigned left = 15740;
        while (left) {
            advance(now);
            if (!lofty_mux.length) lofty_boundary(lofty_mux.published);
            unsigned take = lofty_mux.length-lofty_mux.at, skip = 0;
            if (!lofty_mux.filler) {
                assert(dvb.produced != dvb.consumed);
                unsigned meta = lofty_page_at(dvb.get_index)->words[LOFTY_PAGE_META];
                unsigned at = dvb.page_sample ? dvb.page_sample : meta>>16;
                if (take > (meta&65535)-at) take = (meta&65535)-at;
                skip = at%12;
            }
            // At most one page per event, preserving source/store parity.
            if (take > 1536) take = 1536;
            if (take > left) take = left;
            assert(lofty_shape(dst+(lofty_mux.output&3),lofty_mux.output,take) == ESP_OK);
            now += ((skip+take+11)/12)*23;
            left -= take;
        }
        assert(now <= t+40137);
        advance(now);
        if (blocked) {
            blocked = false;
            // Wake at the fill ceiling. A bulk-map wait still pays bulk cost;
            // first-page FEC was allowed to run before the full-queue wait.
            next = t+40137+(page == 1 ? bulk : page ? 1000 : 2000);
        }
        assert(!dvb.queue_empty);
    }
    assert((argc == 4 || lofty_mux.streaming > 0) && !lofty_mux.startup);
    assert(slow ? lofty_mux.steady > 0 : lofty_mux.steady == 0);
    printf("@{\"test\":\"streaming\",\"modcod\":%u,\"slow\":%u,\"prefill\":%u,\"data\":%u,"
           "\"streamed\":%u,\"startup\":%u,\"steady\":%u,\"threshold\":%u,\"lead\":%lld}\n",
           mc,slow,prefill,lofty_mux.frames,lofty_mux.streaming,lofty_mux.startup,lofty_mux.steady,
           lofty_mux.admission_pages&255,(long long)lofty_mux.head[2]/3);
    free(dst); release(); assert(!live_allocations);
    return 0;
}
