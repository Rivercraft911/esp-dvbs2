/* TS packets to UDP datagrams of seven (w5500_ts.h). */
#include "w5500_ts.h"

#include <string.h>

void w5500_ts_queue_init(w5500_ts_queue *q, uint8_t *slots, uint32_t size)
{
    *q = (w5500_ts_queue){.slots = slots, .size = size};
}

uint32_t w5500_ts_waiting(const w5500_ts_queue *q)
{
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    return head - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}

bool w5500_ts_push(w5500_ts_queue *q, const uint8_t *packet)
{
    const uint32_t head = q->head;
    const uint32_t waiting = head - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (waiting >= q->size) {
        q->dropped++;
        return false;
    }
    memcpy(q->slots + (size_t)(head & (q->size - 1)) * W5500_TS_PACKET, packet, W5500_TS_PACKET);
    __atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE);
    if (waiting + 1 > q->peak) q->peak = waiting + 1;
    return true;
}

/* Up to n packets from the queue into out; the slots go back to the producer at once. */
static unsigned take(w5500_ts_queue *q, uint8_t *out, unsigned n)
{
    const uint32_t tail = q->tail;
    const uint32_t waiting = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) - tail;
    if (n > waiting) n = waiting;
    for (unsigned k = 0; k < n; ++k)
        memcpy(out + k * W5500_TS_PACKET,
               q->slots + (size_t)((tail + k) & (q->size - 1)) * W5500_TS_PACKET, W5500_TS_PACKET);
    __atomic_store_n(&q->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}

void w5500_ts_sender_init(w5500_ts_sender *t, w5500 *chip, unsigned sock, w5500_ts_queue *q,
                          uint8_t *const buf[2], uint32_t (*clock)(void))
{
    *t = (w5500_ts_sender){.chip = chip, .sock = sock, .q = q, .buf = {buf[0], buf[1]},
                           .clock = clock, .wr = chip->sock[sock].tx_wr};
}

int w5500_ts_send(w5500_ts_sender *t, bool partial)
{
    if (t->st.error) return t->st.error;
    for (;;) {
        unsigned n = t->held;
        if (!n) {
            const uint32_t waiting = w5500_ts_waiting(t->q);
            if (!waiting || (waiting < W5500_TS_GROUP && !partial)) return W5500_OK;
            n = W5500_TS_PACKET * take(t->q, t->buf[t->next], W5500_TS_GROUP);
        }
        const uint32_t start = t->clock ? t->clock() : 0;
        const int e = w5500_udp_send(t->chip, t->sock, t->buf[t->next], n);
        const uint32_t after = t->clock ? t->clock() : 0, cycles = after - start;
        if (t->chip->sock[t->sock].tx_wr != t->wr) {   /* this call started a SEND */
            t->wr = t->chip->sock[t->sock].tx_wr;
            t->wr_at = after;
        }
        if (e == W5500_FULL) {
            t->held = (uint16_t)n;
            t->st.full++;
            return e;
        }
        t->held = 0;
        if (e) return t->st.error = e;
        t->st.datagrams++;
        t->st.packets += n / W5500_TS_PACKET;
        t->st.partial += n < W5500_TS_DATAGRAM;
        t->st.send_cycles += cycles;
        if (cycles > t->st.max_send_cycles) t->st.max_send_cycles = cycles;
        t->next ^= 1;
    }
}
