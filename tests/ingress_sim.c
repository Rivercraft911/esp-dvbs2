/* Host model of the GDMA receive channel behind ingress.c, for tests/test_ingress.py.
 *
 * It follows the ESP32-S3 TRM v1.8 behaviour the ring relies on: bytes of one CS-framed
 * transaction go into the current descriptor; a full buffer is written back with owner = 0,
 * length = size and no EOF, and the DMA moves to the next descriptor (section 3.4.1); the end of
 * the transaction writes back the current descriptor with EOF (section 30.5.6.2, Table 30.5-7);
 * a NULL next pointer stops the DMA until software appends (section 3.4.5). Data that arrives
 * with no descriptor is dropped and raises the descriptor-empty event (section 3.5); the model
 * then stays halted until the restart. A transaction that ends exactly at a buffer boundary
 * puts its EOF into the next descriptor with length 0, the less convenient of the two possible
 * behaviours. The model also flags any write into a descriptor the consumer owns. */
#include <stdlib.h>
#include <string.h>

#include "ingress.h"

typedef struct {
    ingress_ring ring;
    ingress_hw hw;
    ingress_desc *cur;      /* next descriptor the DMA writes; NULL: stopped at chain end */
    ingress_desc *last;     /* last descriptor written back */
    uint32_t fill;
    int halted;
    int ready, ready_calls;
    int violations, restarts, appends;
    int isr;                /* run the end-of-frame interrupt (ingress_update_ready) */
    uint32_t buffers[INGRESS_SLOTS * INGRESS_SLOT_BYTES / 4];
} sim;

static void rearm(void *ctx, ingress_desc *slot, bool restart)
{
    sim *s = ctx;
    if (restart) {
        s->cur = slot;
        s->fill = 0;
        s->halted = 0;
        s->ring.overrun = false;
        s->ring.in_long = false;
        s->restarts++;
    } else if (!s->ring.overrun) {
        s->appends++;
        if (!s->cur && s->last && s->last->next) {
            s->cur = s->last->next; /* restart bit: re-read the tail's next pointer */
        }
    }
    ingress_update_ready(&s->ring);
}

static void ready(void *ctx, int level)
{
    sim *s = ctx;
    s->ready = level;
    s->ready_calls++;
}

sim *sim_new(void)
{
    sim *s = calloc(1, sizeof *s);
    s->hw = (ingress_hw){.rearm = rearm, .ready = ready, .ctx = s};
    s->isr = 1;
    ingress_init(&s->ring, (uint8_t *)s->buffers, &s->hw);
    s->cur = &s->ring.desc[0];
    ingress_update_ready(&s->ring);
    return s;
}

void sim_free(sim *s)
{
    free(s);
}

ingress_ring *sim_ring(sim *s)
{
    return &s->ring;
}

void sim_set_isr(sim *s, int on)
{
    s->isr = on;
}

void sim_stats(sim *s, ingress_stats *out)
{
    *out = s->ring.st;
}

int sim_get(sim *s, int what)
{
    switch (what) {
    case 0: return s->ready;
    case 1: return s->violations;
    case 2: return s->restarts;
    case 3: return s->appends;
    case 4: return s->halted;
    default: return -1;
    }
}

static void writeback(sim *s, int eof)
{
    ingress_desc *d = s->cur;
    d->dw0 = (d->dw0 & 0xfffu) | s->fill << 12 | (eof ? INGRESS_EOF : 0);
    s->last = d;
    s->cur = d->next;
    s->fill = 0;
}

static int take(sim *s)
{
    if (s->halted || !s->cur) {
        s->halted = 1;
        s->ring.overrun = true;
        s->ring.st.overruns++;
        ingress_update_ready(&s->ring);
        return 0;
    }
    if (s->fill == 0 && !(s->cur->dw0 & INGRESS_OWNER_DMA)) {
        s->violations++;
    }
    return 1;
}

/* One CS assertion with n bytes. Returns the number of bytes stored. */
uint32_t sim_transfer(sim *s, const uint8_t *data, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n; ++i) {
        if (!take(s)) {
            return i; /* the rest of this transaction is lost */
        }
        s->cur->buf[s->fill++] = data[i];
        if (s->fill == (s->cur->dw0 & 0xfffu)) {
            writeback(s, 0);
        }
    }
    if (take(s)) {
        writeback(s, 1);
        if (s->isr) {
            ingress_update_ready(&s->ring);
        }
    }
    return i;
}

void sim_type(sim *s, unsigned type) { s->ring.type = type; }
unsigned sim_wrong_type(sim *s) { return s->ring.wrong_type; }
