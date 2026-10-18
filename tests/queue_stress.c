// Real encoder/consumer and notification wait, without a hardware timing claim.
#include "native.h"
static unsigned checked_take(int, unsigned);
#define ulTaskNotifyTake checked_take
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#undef ulTaskNotifyTake
#include <sched.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned armed, paused, resume_task, early_notifications;
static uint32_t pause_put, pause_get;

static unsigned checked_take(int clear, unsigned timeout)
{
    pthread_mutex_lock(&gate);
    if (armed) {
        armed = 0;
        pause_put = dvb.produced;
        pause_get = __atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE);
        assert(pause_put-pause_get == dvb.queue_pages);
        paused = 1;
        pthread_cond_broadcast(&changed);
        while (!resume_task) pthread_cond_wait(&changed,&gate);
        resume_task = paused = 0;
        pthread_mutex_lock(&current->mutex);
        assert(current->notifications); // bank wake happened before the take
        pthread_mutex_unlock(&current->mutex);
        ++early_notifications;
    }
    pthread_mutex_unlock(&gate);
    return ulTaskNotifyTake(clear,timeout);
}

static uint32_t checksum(uint32_t h, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    while (bytes--) h = (h^*p++)*16777619u;
    return h;
}

static void full_queue(void)
{
    TickType_t end = xTaskGetTickCount()+5000;
    while (__atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed != dvb.queue_pages ||
           !__atomic_load_n(&dvb.waiting,__ATOMIC_ACQUIRE)) {
        assert((int32_t)(xTaskGetTickCount()-end) < 0);
        sched_yield();
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    unsigned mc = atoi(argv[1]);
    uint32_t x = atoi(argv[2]);
    callsign = "N0CALL";
    assert(start_encoder("dvbpace",mc,128,0,3,80000000));
    unsigned pages = 0, wraps = 0, sequence_wraps = 0, banks_done = 0;
    unsigned last_index = dvb.get_index;
    uint32_t last_sequence = dvb.consumed, waveform_hash = 2166136261u;
    const unsigned slices[] = {1,11,12,47,512,1536};
    full_queue();
    while (pages < 4096) {
        unsigned bank_pages = banks_done%3 ? 10 : 11;
        if (bank_pages > 4096-pages) bank_pages = 4096-pages;
        const unsigned sent = enc.task->notifications_sent;
        for (unsigned p = 0; p < bank_pages; ++p) {
            uint32_t get = dvb.consumed, put = __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE);
            assert(put-get > 0 && put-get <= dvb.queue_pages);
            lofty_page *page = lofty_page_at(dvb.get_index);
            uint32_t page_hash = checksum(2166136261u,page,sizeof *page);
            do {
                sched_yield(); // consumer retains a partial page across switches
                assert(page_hash == checksum(2166136261u,page,sizeof *page));
                x ^= x<<13; x ^= x>>17; x ^= x<<5;
                size_t n = slices[x%6], left = MACHINE_PAGE_SAMPLES-dvb.page_sample;
                if (n > left) n = left;
                // No wait/retry inside this consumer, including bank bursts.
                assert(moon_shape(reference,dvb.queue_sample,n) == ESP_OK);
                waveform_hash = checksum(waveform_hash,reference,4*n);
            } while (dvb.page_sample);
            ++pages;
            if (dvb.get_index < last_index) ++wraps;
            last_index = dvb.get_index;
            if (dvb.consumed < last_sequence) ++sequence_wraps;
            last_sequence = dvb.consumed;
        }
        assert(dvb.wake_pending && enc.task->notifications_sent == sent);
        // The producer sees the bank's releases, but sleeps until its one wake.
        assert(dvb.produced-dvb.consumed == dvb.queue_pages-bank_pages);
        pthread_mutex_lock(&gate);
        unsigned suspended = paused;
        if (suspended) {
            assert(pause_put-pause_get == dvb.queue_pages);
            assert(pause_put == dvb.produced && dvb.consumed-pause_get == bank_pages);
        }
        if (!suspended && banks_done%16 == 0) armed = 1;
        moon_wake();
        dvb.wake_pending = false;
        assert(enc.task->notifications_sent == sent+1);
        if (suspended) { resume_task = 1; pthread_cond_broadcast(&changed); }
        if (armed) while (!paused) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
        ++banks_done;
        // Host-only pacing: verify progress after the wake before the next
        // unpaced bank. This cannot establish a silicon deadline or lead.
        full_queue();
    }
    assert(!dvb.queue_empty && wraps >= 75 && sequence_wraps == 1 && early_notifications >= 20);
    stop_encoder(); printf("}\n"); // stop must wake and join a full sleeping producer
    assert(live_allocations == 0);
    printf("@{\"test\":\"queue_stress\",\"modcod\":%u,\"pages\":%u,\"wraps\":%u,"
           "\"banks\":%u,\"early_notifications\":%u,\"waveform_hash\":%u}\n",
           mc,pages,wraps,banks_done,early_notifications,(unsigned)waveform_hash);
}
