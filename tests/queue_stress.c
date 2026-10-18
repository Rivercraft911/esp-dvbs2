// The real encoder/page consumer, with a deterministic peer-task preemption.
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#include <sched.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned armed, paused, resume_task, requested, peer_priority, progressed, initial_frames;
static uint32_t clock_cycles, pause_put, pause_get, pause_phase;

uint32_t queue_stress_clock(void)
{
    if (current && __atomic_load_n(&armed,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&dvb.producer_phase,__ATOMIC_ACQUIRE) == requested &&
        uxTaskPriorityGet(NULL) <= peer_priority && __atomic_exchange_n(&armed,0,__ATOMIC_ACQ_REL)) {
        pthread_mutex_lock(&gate);
        pause_put = dvb.produced;
        pause_get = __atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE);
        pause_phase = requested;
        paused = 1;
        pthread_cond_broadcast(&changed);
        while (!resume_task) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
    }
    if (current && resume_task && enc.frames > initial_frames)
        __atomic_store_n(&progressed,1,__ATOMIC_RELEASE);
    return __atomic_add_fetch(&clock_cycles,17,__ATOMIC_RELAXED);
}

static uint32_t checksum(uint32_t h, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    while (bytes--) h = (h^*p++)*16777619u;
    return h;
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    unsigned mc = atoi(argv[1]);
    requested = atoi(argv[2]);
    peer_priority = atoi(argv[3]);
    unsigned expect_fault = atoi(argv[4]);
    callsign = "N0CALL";
    assert(start_encoder("dvbpace",mc,128,0,3,80000000));
    const unsigned priority = enc.priority;
    initial_frames = enc.frames;
    __atomic_store_n(&armed,1,__ATOMIC_RELEASE);
    if (expect_fault && requested == QUEUE_WAIT) {
        pthread_mutex_lock(&gate);
        while (!paused) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
        assert(pause_put-pause_get == dvb.queue_pages);
    }
    unsigned pages = 0, wraps = 0, sequence_wraps = 0, last_index = dvb.get_index, idle = 0;
    uint32_t last_sequence = dvb.consumed;
    uint32_t x = 2026+requested, page_hash = 0, waveform_hash = 2166136261u;
    const unsigned slices[] = {1,11,12,47,512,1536};
    while (pages < 4096) {
        pthread_mutex_lock(&gate);
        unsigned suspended = paused;
        pthread_mutex_unlock(&gate);
        uint32_t get = dvb.consumed, put = __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE);
        uint32_t ready = put-get;
        assert(ready <= dvb.queue_pages);
        if (!ready) {
            if (!suspended) { assert(++idle < 10000000); sched_yield(); continue; }
            // Fail exactly as the real fill would. Its consumer never sleeps.
            assert(moon_shape(reference,dvb.queue_sample,12) == ESP_FAIL);
            assert(expect_fault && dvb.queue_fault.put == dvb.queue_fault.get);
            printf("@{\"modcod\":%u,\"priority\":%u,\"phase\":%u,"
                   "\"producer_put\":%u,\"producer_get\":%u,\"producer_ready\":%u,"
                   "\"consumer_put\":%u,\"consumer_get\":%u,\"consumer_ready\":0}\n",
                   mc,priority,pause_phase,pause_put,pause_get,pause_put-pause_get,put,get);
            // A peer ran for longer than the queue's coverage. A kernel-stage
            // pause must remain in that stage's timing, unlike full-queue waits.
            __atomic_add_fetch(&clock_cycles,1219709,__ATOMIC_RELAXED);
            pthread_mutex_lock(&gate);
            resume_task = 1;
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&gate);
            while (!__atomic_load_n(&progressed,__ATOMIC_ACQUIRE)) sched_yield();
            break;
        }
        idle = 0;
        lofty_page *page = lofty_page_at(dvb.get_index);
        if (!dvb.page_sample) page_hash = checksum(2166136261u,page,sizeof *page);
        // Yield while owning a partial page. Its contents must not change.
        sched_yield();
        assert(page_hash == checksum(2166136261u,page,sizeof *page));
        x ^= x<<13; x ^= x>>17; x ^= x<<5;
        size_t n = slices[x%6], left = MACHINE_PAGE_SAMPLES-dvb.page_sample;
        if (n > left) n = left;
        assert(moon_shape(reference,dvb.queue_sample,n) == ESP_OK);
        waveform_hash = checksum(waveform_hash,reference,4*n);
        if (!dvb.page_sample) {
            ++pages;
            if (dvb.get_index < last_index) ++wraps;
            last_index = dvb.get_index;
            if (dvb.consumed < last_sequence) ++sequence_wraps;
            last_sequence = dvb.consumed;
        }
    }
    if (!expect_fault) assert(pages == 4096 && !paused && !dvb.queue_empty && wraps >= 75 && sequence_wraps == 1);
    else assert(paused && dvb.queue_empty == 1);
    stop_encoder(); printf("}\n");
    assert(live_allocations == 0 && native_watch_add == native_watch_delete && native_control_priority == 1);
    assert(dvb.queue_wakes == 0);
    if (expect_fault) {
        unsigned stage = requested == QUEUE_PL ? 5 : 4;
        if (requested == QUEUE_WAIT) {
            assert(dvb.queue_wait_max >= 1219709 && enc.stage[4].max_cycles < 1219709);
        } else assert(enc.stage[stage].max_cycles >= 1219709);
    }
    printf("@{\"test\":\"queue_stress\",\"pages\":%u,\"wraps\":%u,\"priority\":%u,\"fault\":%u,\"waveform_hash\":%u}\n",
           pages,wraps,priority,expect_fault,(unsigned)waveform_hash);
}
