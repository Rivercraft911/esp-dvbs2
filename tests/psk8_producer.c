// Actual FEC order and publication clocks, including a cold accounting path.
#include "native.h"
#include <stdbool.h>
#include "dvbs2.h"
static uint32_t clock_now;
static bool timed;
static uint32_t cycle_clock(void) { return timed ? clock_now : 17; }
static void store_clock(const volatile void *);
static unsigned held_take(int, unsigned);
static int checked_bb(const dvbs2_fast *, dvbs2_workspace *, const uint8_t *, size_t);
static void checked_bch(dvbs2_fast *, dvbs2_workspace *);
static void checked_ldpc(dvbs2_fast *, dvbs2_workspace *);
#define esp_cpu_get_cycle_count cycle_clock
#define __atomic_store_n(p,v,o) (store_clock(p),__atomic_store_n(p,v,o))
#define ulTaskNotifyTake held_take
#define dvbs2_fast_bb_gcs checked_bb
#define dvbs2_fast_bch checked_bch
#define dvbs2_fast_ldpc checked_ldpc
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#undef dvbs2_fast_bb_gcs
#undef dvbs2_fast_bch
#undef dvbs2_fast_ldpc
#undef ulTaskNotifyTake
#undef __atomic_store_n
#undef esp_cpu_get_cycle_count

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool paused, resume_task, bb_full, ldpc_full;
static unsigned stage, fec_completed;

static int checked_bb(const dvbs2_fast *f, dvbs2_workspace *w, const uint8_t *p, size_t n)
{
    assert(stage == 0 || stage == 3); stage = 1;
    bb_full = dvb.produced-dvb.consumed == dvb.queue_pages;
    return dvbs2_fast_bb_gcs(f,w,p,n);
}
static void checked_bch(dvbs2_fast *f, dvbs2_workspace *w)
{
    assert(stage == 1); stage = 2;
    dvbs2_fast_bch(f,w);
}
static void checked_ldpc(dvbs2_fast *f, dvbs2_workspace *w)
{
    assert(stage == 2); stage = 3;
    ldpc_full = dvb.produced-dvb.consumed == dvb.queue_pages;
    dvbs2_fast_ldpc(f,w); ++fec_completed;
}
static unsigned held_take(int clear, unsigned timeout)
{
    pthread_mutex_lock(&gate);
    paused = true; pthread_cond_signal(&changed);
    while (!resume_task) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
    return ulTaskNotifyTake(clear,timeout);
}
static void store_clock(const volatile void *p)
{
    // The old publisher counted this cold calculation as its next page gap.
    if (timed && p == &lofty_mux.admission_pages) clock_now += 13500;
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    unsigned mc = atoi(argv[1]);
    callsign = "N0CALL";
    // The pace repeats its prepared frame (--fixture); the dump encodes the xorshift stream.
    assert(start_encoder_options(argc == 3 ? "dvbdump" : "dvbpace",mc,128,0,3,80000000,false,argc == 2));
    pthread_mutex_lock(&gate);
    while (!paused) pthread_cond_wait(&changed,&gate);
    assert(dvb.produced-dvb.consumed == dvb.queue_pages);
    assert(dvb.producer_phase == QUEUE_WAIT);
    assert(fec_completed-enc.warmup_frames == enc.frames+1);
    assert(enc.stage[3].calls == enc.frames+1);
    dvbs2_workspace expected;
    uint8_t payload[8100]; uint32_t seed = SEED;
    const size_t bytes = (enc.mode->kbch-80)/8;
    for (unsigned f = 0; f < (argc == 3 ? enc.frames+1 : 1); ++f)
        payload_fill(payload,bytes,&seed);
    station_payload_header(payload,bytes,callsign);
    assert(!dvbs2_bb_gcs(&expected,enc.mode,payload,8*bytes));
    dvbs2_bb_scramble(&expected,enc.mode);
    dvbs2_bch(&expected,enc.mode); dvbs2_ldpc(&expected,enc.mode);
    assert(!memcmp(expected.bits,enc.ws->bits,DVBS2_FEC_BYTES));
#ifdef QUEUE_TEST_PAGES
    assert(bb_full && ldpc_full); // even with zero spare pages, FEC runs first
#endif
    printf("@{\"test\":\"fec_order\",\"modcod\":%u,\"pages\":%u,\"fec_ahead\":1,"
           "\"bb_while_full\":%u,\"ldpc_while_full\":%u}\n",mc,dvb.queue_pages,bb_full,ldpc_full);
    enc.stop = true; resume_task = true;
    pthread_cond_signal(&changed); pthread_mutex_unlock(&gate);
    xTaskNotifyGive(enc.task); xSemaphoreTake(enc.exited,portMAX_DELAY);
    vTaskDelete(enc.task); enc.task = NULL;

    dvb.put_index = 0; dvb.produced = dvb.consumed = 0;
    dvb.prepare_pages = 0; dvb.page_clock = 0;
    dvb.page_wait = dvb.queue_wait_cycles = 0;
    lofty_mux.page_max = UINT32_MAX;
#ifndef LOFTY_OLD_TIMING
    lofty_mux.admission_gap = UINT32_MAX;
#endif
    lofty_mux.admission_pages = 44 | (44<<8);
    clock_now = UINT32_MAX-10000; timed = true;
    for (unsigned frame = 0; frame < 2; ++frame) {
        for (unsigned page = 0; page < 44; ++page) {
            unsigned prefix = (frame&1)*6;
            lofty_mux.skip = page ? 0 : prefix;
            dvb.mapper.at = page == 43 ? 22194 : (page+1)*512-prefix/3;
            clock_now += 1000;
            lofty_publish(page == 43 ? 534+prefix : 1536);
            // An immediate consumer releases ownership without altering time.
            dvb.consumed = dvb.produced;
        }
        dvb.mapper.pending = 0;
        assert(!lofty_finish());
    }
    printf("@{\"test\":\"publication_clock\",\"gap\":%u,\"active_pages\":%u,\"wait_pages\":%u}\n",
           lofty_mux.page_max,lofty_mux.admission_pages&255,lofty_mux.admission_pages>>8);
    fflush(stdout);
    assert(lofty_mux.page_max == 1000);
    assert((lofty_mux.admission_pages&255) == 10 && (lofty_mux.admission_pages>>8) == 26);
#ifndef LOFTY_OLD_TIMING
    assert(lofty_mux.admission_max == 13500);
    lofty_mux.rate_misses = 0;
    clock_now += 5000; dvb.mapper.at = 1536;
    lofty_publish(1536); dvb.consumed = dvb.produced;
    assert(!lofty_finish());
    assert(lofty_mux.page_max == 5000 && lofty_mux.rate_misses == 1);
    assert((lofty_mux.admission_pages&255) == lofty_admission(5000,0));
    for (unsigned page = 1; page <= 3; ++page) {
        clock_now += 1000; dvb.mapper.at = 512*page;
        lofty_publish(1536); dvb.consumed = dvb.produced;
    }
    assert(lofty_mux.page_max == 5000); // real stalls remain, accounting does not leak back in
#endif
    timed = false; release(); assert(run_returned());
    return 0;
}
