// Frame ring between the encoder and the bank fill (ring.h). Portable C11: tests/test_ring.py
// builds it for the host and runs it against a model of the bank schedule.

#include "ring.h"

#include <string.h>

#include "esp_attr.h"

void ring_init(ring_t *r, uint8_t *slots, uint32_t nslots, const uint8_t *filler,
               uint32_t frame_symbols)
{
    memset(r, 0, sizeof *r);
    r->slots = slots;
    r->nslots = nslots;
    r->filler = filler;
    r->frame_symbols = r->filler_symbols = frame_symbols;
    r->min_lead = INT64_MAX;
}

uint8_t *ring_slot(ring_t *r, uint32_t e)
{
    // The acquire pairs with the fill's release of `freed`: its reads of the slot are done.
    if (e - __atomic_load_n(&r->freed, __ATOMIC_ACQUIRE) >= r->nslots) {
        return NULL;
    }
    return r->slots + (size_t)(e % r->nslots) * r->frame_symbols;
}

void ring_publish(ring_t *r, uint32_t e)
{
    __atomic_store_n(&r->published, e + 1, __ATOMIC_RELEASE);
}

void ring_filler_length(ring_t *r, uint32_t symbols)
{
    r->filler_symbols = symbols;
}

// Copy absolute symbols, retaining choices until no future overlap can read them.
static IRAM_ATTR int ring_read(ring_t *r, uint8_t *dst, int64_t first, size_t count)
{
    const uint32_t published = __atomic_load_n(&r->published, __ATOMIC_ACQUIRE);
    const int64_t end = first + (int64_t)count;
    if (first < 0) {
        const size_t zeros = end < 0 ? count : (size_t)-first;
        memset(dst, 0, zeros);
        dst += zeros, count -= zeros, first = 0;
    }
    if (!count) {
        return 0;
    }
    if ((uint64_t)first < (r->release_position == r->next_position ? r->chosen_end :
                           r->start[r->release_position % RING_CHOICES])) {
        return -1;
    }
    uint32_t p = r->release_position;
    while (p < r->next_position && r->end[p % RING_CHOICES] <= (uint64_t)first) {
        ++p;
    }
    while (r->chosen_end < (uint64_t)end) {
        if (r->next_position - r->release_position >= RING_CHOICES) {
            return -1;
        }
        const unsigned i = r->next_position++ % RING_CHOICES;
        uint32_t c = RING_FILLER;
        if (r->next_frame != published) {
            c = r->next_frame++;
        } else {
            r->underruns++;
        }
        r->choice[i] = c;
        r->start[i] = r->chosen_end;
        r->chosen_end += c == RING_FILLER ? r->filler_symbols : r->frame_symbols;
        r->end[i] = r->chosen_end;
    }
    const int64_t lead = (int64_t)r->chosen_end +
                         (int64_t)(published - r->next_frame) * r->frame_symbols - end;
    if (lead < r->min_lead) {
        r->min_lead = lead;
    }
    while (count) {
        const unsigned i = p++ % RING_CHOICES;
        const uint32_t c = r->choice[i];
        const uint8_t *src = c == RING_FILLER ? r->filler :
                            r->slots + (size_t)(c % r->nslots) * r->frame_symbols;
        const size_t at = (uint64_t)first - r->start[i];
        const size_t avail = r->end[i] - (uint64_t)first;
        const size_t part = avail < count ? avail : count;
        memcpy(dst, src + at, part);
        dst += part, count -= part, first += part;
    }
    return 0;
}

static IRAM_ATTR void ring_release(ring_t *r, int64_t oldest)
{
    uint32_t freed = 0;
    while (r->release_position < r->next_position &&
           r->end[r->release_position % RING_CHOICES] <= (uint64_t)oldest) {
        const uint32_t c = r->choice[r->release_position++ % RING_CHOICES];
        if (c != RING_FILLER) {
            freed = c + 1;
        }
    }
    if (freed) {
        __atomic_store_n(&r->freed, freed, __ATOMIC_RELEASE);
    }
}

IRAM_ATTR int ring_window(ring_t *r, uint8_t *dst, size_t room, uint64_t first, size_t count,
                          unsigned sps, unsigned history, unsigned bank_words)
{
    const uint64_t s0 = first / sps;
    const size_t nsym = (size_t)((first + count + sps - 1) / sps - s0);
    if (history + nsym > room || ring_read(r, dst, (int64_t)s0 - history, history + nsym)) {
        return -1;
    }
    const uint64_t earliest = first + count > bank_words ? first + count - bank_words : 0;
    const int64_t oldest = (int64_t)(earliest / sps) - history;
    if (oldest > 0) {
        ring_release(r, oldest);
    }
    return (int)nsym;
}
