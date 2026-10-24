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

void w5500_ts_sender_init(w5500_ts_sender *t, w5500 *chip, const unsigned *socks, unsigned n,
                          w5500_ts_queue *q, uint8_t *const buf[2], uint32_t (*clock)(void))
{
    *t = (w5500_ts_sender){.chip = chip, .socks = n == 2 ? 2 : 1, .q = q,
                           .buf = {buf[0], buf[1]}, .clock = clock};
    for (unsigned k = 0; k < 2; ++k) {
        t->sock[k] = socks[k < t->socks ? k : 0];
        t->wr[k] = chip->sock[t->sock[k]].tx_wr;
    }
}

/* A call that moved the socket's Sn_TX_WR gave a SEND: note when. */
static void note(w5500_ts_sender *t, unsigned k, uint32_t at)
{
    const uint16_t wr = t->chip->sock[t->sock[k]].tx_wr;
    if (wr == t->wr[k]) return;
    t->wr[k] = wr;
    t->wr_at[k] = at;
}

int w5500_ts_send_other(w5500_ts_sender *t, const uint8_t *buf, size_t n, uint16_t port)
{
    if (t->st.error) return t->st.error;
    if (t->socks != 2 || t->held) return t->socks != 2 ? W5500_ERR_ARG : W5500_FULL;
    const unsigned k = t->next, s = t->sock[k];
    w5500_socket *so = &t->chip->sock[s];
    if (so->flight || so->q_count) {
        const int e = w5500_udp_poll(t->chip, s);
        note(t, k, t->clock ? t->clock() : 0);
        if (e < 0) return t->st.error = e;
        if (e) return W5500_FULL;
    }
    uint8_t ip[4];
    const uint16_t back = so->dest_port;
    memcpy(ip, so->dest, 4);
    w5500_udp_dest(t->chip, s, ip, port);
    int e = w5500_udp_send(t->chip, s, buf, n);
    if (e == W5500_OK) e = w5500_udp_poll(t->chip, s);   /* its SEND now, with this port */
    note(t, k, t->clock ? t->clock() : 0);
    w5500_udp_dest(t->chip, s, ip, back);               /* applied before the socket's next SEND */
    if (e < 0) return t->st.error = e;
    t->next ^= 1;
    return W5500_OK;
}

int w5500_ts_send(w5500_ts_sender *t, bool partial)
{
    if (t->st.error) return t->st.error;
    for (;;) {
        const unsigned k = t->socks == 2 ? t->next : 0, s = t->sock[k];
        const w5500_socket *so = &t->chip->sock[s];
        unsigned n = t->held;
        if (!n) {
            const uint32_t waiting = w5500_ts_waiting(t->q);
            if (!waiting || (waiting < W5500_TS_GROUP && !partial)) return W5500_OK;
        }
        if (t->socks == 2 && (so->flight || so->q_count)) {   /* datagram k - 2 still going */
            const int e = w5500_udp_poll(t->chip, s);
            note(t, k, t->clock ? t->clock() : 0);
            if (e < 0) return t->st.error = e;
            if (e) {
                t->st.full++;
                return W5500_FULL;
            }
        }
        if (!n) n = W5500_TS_PACKET * take(t->q, t->buf[t->next], W5500_TS_GROUP);
        const uint32_t start = t->clock ? t->clock() : 0;
        int e = w5500_udp_send(t->chip, s, t->buf[t->next], n);
        const uint32_t after = t->clock ? t->clock() : 0, cycles = after - start;
        note(t, k, after);
        if (t->socks == 2 && e == W5500_OK) {   /* the SEND now, so the chip holds it ready */
            e = w5500_udp_poll(t->chip, s);
            note(t, k, t->clock ? t->clock() : 0);
            e = e < 0 ? e : W5500_OK;
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
