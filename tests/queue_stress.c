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
    setenv("UNPACED_LOFTY","1",1);
    callsign = "N0CALL";
    assert(start_encoder("dvbpace",mc,128,0,3,80000000));
    full_queue();
    // This ownership test suspends the producer indefinitely. That exceeds
    // every finite streaming rate, so exercise the measured-slow fallback.
    lofty_mux.page_max = 1000000;
    lofty_mux.admission_pages = lofty_admission(lofty_mux.page_max,0) | (lofty_admission(lofty_mux.page_max,47220)<<8);
    assert(lofty_mux.admission_pages == (LOFTY_FRAME_PAGES | (LOFTY_FRAME_PAGES<<8)));
    armed = 1;
    unsigned pages = 0, wraps = 0, sequence_wraps = 0, bank_pages = 0;
    unsigned last_index = dvb.get_index;
    uint32_t last_sequence = dvb.consumed, data_hash = 2166136261u, page_hash = 0;
    const unsigned slices[] = {1,11,12,47,512,1536};
    TickType_t deadline = xTaskGetTickCount()+15000;
    while (pages < 4096) {
        if ((int32_t)(xTaskGetTickCount()-deadline) >= 0) {
            fprintf(stderr,"stalled pages=%u ready=%u frames=%u put=%u get=%u wait=%u paused=%u armed=%u dummy=%u\n",pages,lofty_mux.published,lofty_mux.frames,dvb.produced,dvb.consumed,dvb.waiting,paused,armed,lofty_mux.startup+lofty_mux.steady);
            abort();
        }
        if (!lofty_mux.length) lofty_boundary(__atomic_load_n(&lofty_mux.published,__ATOMIC_ACQUIRE));
        unsigned filler = lofty_mux.filler;
        size_t left = lofty_mux.length-lofty_mux.at;
        lofty_page *page = lofty_page_at(dvb.get_index);
        if (!filler) {
            unsigned meta = page->words[LOFTY_PAGE_META];
            unsigned at = dvb.page_sample ? dvb.page_sample : meta>>16;
            if (left > (meta&65535)-at) left = (meta&65535)-at;
            if (!dvb.page_sample) {
                page_hash = checksum(2166136261u,page,sizeof *page);
                data_hash = checksum(data_hash,page->words,4*(((meta&65535)+11)/12));
                data_hash = checksum(data_hash,&meta,sizeof meta);
            }
            sched_yield();
            assert(page_hash == checksum(2166136261u,page,sizeof *page));
        }
        x ^= x<<13; x ^= x>>17; x ^= x<<5;
        size_t n = slices[x%6];
        if (n > left) n = left;
        assert(lofty_shape(reference,lofty_mux.output,n) == ESP_OK);
        if (!filler && !dvb.page_sample) {
            ++pages; ++bank_pages;
            if (dvb.get_index < last_index) ++wraps;
            last_index = dvb.get_index;
            if (dvb.consumed < last_sequence) ++sequence_wraps;
            last_sequence = dvb.consumed;
            if (bank_pages == 10) {
                moon_wake(); dvb.wake_pending = false; bank_pages = 0;
                pthread_mutex_lock(&gate);
                if (pages == 10) {
                    while (!paused) pthread_cond_wait(&changed,&gate);
                    assert(pause_put-pause_get == dvb.queue_pages);
                }
                pthread_mutex_unlock(&gate);
            }
        }
        pthread_mutex_lock(&gate);
        if (paused && !resume_task && lofty_mux.startup+lofty_mux.steady >= 6) {
            // The consumer has continued through six complete-frame choices
            // while the producer cannot run. Its queued partial data stays owned.
            assert(dvb.produced-dvb.consumed < LOFTY_FRAME_PAGES);
            resume_task = 1;
            pthread_cond_broadcast(&changed);
        }
        pthread_mutex_unlock(&gate);
    }
    assert(!dvb.queue_empty && wraps >= 75 && sequence_wraps == 1 && early_notifications == 1);
    stop_encoder(); printf("}\n");
    assert(live_allocations == 0);
    printf("@{\"test\":\"queue_stress\",\"modcod\":%u,\"pages\":%u,\"wraps\":%u,"
           "\"early_notifications\":%u,\"data_hash\":%u}\n",
           mc,pages,wraps,early_notifications,(unsigned)data_hash);
}
