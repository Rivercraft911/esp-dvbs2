/* Hold BBFRAMEs behind outstanding decodes. Only the caller touches this queue or its output. */
#include "dvbs2rx.h"

#include <assert.h>
#include <string.h>

#define WAITING (-2)

typedef struct {
    uint32_t sequence;
    int length;
    unsigned repaired;
    uint8_t bytes[RX_HARD_BYTES];
} held_t;

struct rx_order {
    unsigned capacity, head, count;
    rx_order_emit_fn emit;
    void *ctx;
    rx_order_stats_t st;
    held_t frame[];
};

size_t rx_order_size(unsigned frames)
{
    return sizeof(rx_order_t) + frames * sizeof(held_t);
}

void rx_order_init(rx_order_t *o, unsigned frames, rx_order_emit_fn emit, void *ctx)
{
    assert(frames && emit);
    memset(o, 0, sizeof *o);
    o->capacity = frames;
    o->emit = emit;
    o->ctx = ctx;
}

void rx_order_drain(rx_order_t *o)
{
    while (o->count) {
        held_t *f = &o->frame[o->head];
        if (f->length == WAITING) break;
        o->st.repaired += f->repaired;
        o->emit(o->ctx, f->sequence, f->length >= 0 ? f->bytes : NULL, f->length);
        o->head = (o->head + 1) % o->capacity;
        --o->count;
    }
}

static void push(rx_order_t *o, uint32_t sequence, uint8_t *bbframe, int length)
{
    rx_order_drain(o);
    if (o->count == o->capacity) {
        /* Drain stopped at the oldest waiting frame. Give it up, then use the freed space. */
        o->frame[o->head].length = -1;
        ++o->st.given_up;
        rx_order_drain(o);
    }
    if (!o->count && length != WAITING) {
        o->emit(o->ctx, sequence, bbframe, length);  /* the clean path does not copy */
        return;
    }
    held_t *f = &o->frame[(o->head + o->count) % o->capacity];
    f->sequence = sequence;
    f->length = length;
    f->repaired = 0;
    if (length >= 0) memcpy(f->bytes, bbframe, 10U + (unsigned)length);
    ++o->st.held;
    if (++o->count > o->st.max_held) o->st.max_held = o->count;
}

void rx_order_frame(rx_order_t *o, uint32_t sequence, uint8_t *bbframe, int length)
{
    assert(length >= -1 && length <= (int)RX_HARD_BYTES - 10 && (length < 0 || bbframe));
    push(o, sequence, length >= 0 ? bbframe : NULL, length);
}

void rx_order_wait(rx_order_t *o, uint32_t sequence)
{
    push(o, sequence, NULL, WAITING);
}

int rx_order_result(rx_order_t *o, uint32_t sequence, const uint8_t *bbframe, int length)
{
    assert(length >= -1 && length <= (int)RX_HARD_BYTES - 10 && (length < 0 || bbframe));
    for (unsigned i = 0; i < o->count; ++i) {
        held_t *f = &o->frame[(o->head + i) % o->capacity];
        if (f->sequence != sequence || f->length != WAITING) continue;
        f->length = length;
        f->repaired = length >= 0;
        if (length >= 0) memcpy(f->bytes, bbframe, 10U + (unsigned)length);
        return 1;
    }
    return 0;                   /* given up already: its USB record still belongs to the caller */
}

void rx_order_stats(const rx_order_t *o, rx_order_stats_t *stats)
{
    *stats = o->st;
}
