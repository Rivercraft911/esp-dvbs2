#ifndef W5500_TS_H
#define W5500_TS_H

/* A transport stream out through a W5500 UDP socket: a bounded queue of 188-byte TS packets that
 * one task fills and another drains into datagrams of seven packets (1,316 bytes, the usual size
 * for TS over UDP, which ffplay and VLC read). Portable, so tests/test_w5500.py runs it against
 * the fake chip.
 *
 * The producer never waits: a packet that finds the queue full is dropped and counted. The queue
 * is single producer, single consumer: head is written only by the producer, tail only by the
 * consumer, each published with release ordering after the slots it covers, so the two may run
 * on different cores without a lock. */

#include <stdbool.h>
#include <stdint.h>
#include "w5500.h"

#define W5500_TS_PACKET 188
#define W5500_TS_GROUP 7
#define W5500_TS_DATAGRAM (W5500_TS_GROUP * W5500_TS_PACKET)

typedef struct {
    uint8_t *slots;                 /* size x 188 bytes of caller storage; PSRAM is fine */
    uint32_t size;                  /* packets, a power of two */
    uint32_t head, tail;            /* free-running: packets pushed, packets taken */
    uint32_t dropped, peak;         /* producer: packets refused, most ever waiting */
} w5500_ts_queue;

void w5500_ts_queue_init(w5500_ts_queue *q, uint8_t *slots, uint32_t size);
/* Producer: copy one packet in, or count it dropped. */
bool w5500_ts_push(w5500_ts_queue *q, const uint8_t *packet);
/* Packets waiting. */
uint32_t w5500_ts_waiting(const w5500_ts_queue *q);

typedef struct {
    uint32_t datagrams, packets;    /* handed to the chip */
    uint32_t partial;               /* datagrams of fewer than seven packets */
    uint32_t full;                  /* sends the chip refused for lack of TX room, retried */
    uint32_t max_send_cycles;       /* slowest w5500_udp_send that took a datagram */
    uint64_t send_cycles;           /* all of them */
    int error;                      /* the driver error that stopped the sender, or 0 */
} w5500_ts_stats;

typedef struct {
    w5500 *chip;
    unsigned sock[2], socks;        /* the sockets used in turn: one, or two */
    w5500_ts_queue *q;
    uint8_t *buf[2];                /* W5500_TS_DATAGRAM bytes each, DMA capable (w5500_udp_send) */
    uint32_t (*clock)(void);        /* a cycle counter for the send times, or NULL */
    unsigned next;                  /* the buffer (and with two sockets, the socket) next */
    uint16_t held;                  /* bytes in buf[next] that the chip refused, 0 if none */
    uint16_t wr[2];                 /* each socket's Sn_TX_WR after the last SEND seen to start */
    uint32_t wr_at[2];              /* clock() just after that call: about when the SEND began */
    w5500_ts_stats st;
} w5500_ts_sender;

/* Consumer side, on open sockets with a destination. The two buffers alternate because a posted
 * payload may still be clocking out of the last one (w5500_udp_send).
 * With one socket the chip sends a datagram, and the next starts only when the sender next
 * calls the driver. With two, datagram k goes to socket k mod 2 once that socket's datagram
 * k - 2 has its SEND_OK, and gets its SEND at once: the chip holds it while k - 1 is on the
 * wire and sends it straight after (the W5500 sends one frame at a time), so the wire never waits
 * for the sender. The order on the wire is the order of the SENDs, which is k. */
void w5500_ts_sender_init(w5500_ts_sender *t, w5500 *chip, const unsigned *socks, unsigned n,
                          w5500_ts_queue *q, uint8_t *const buf[2], uint32_t (*clock)(void));
/* Send every full datagram waiting, and with partial the rest as one shorter datagram too.
 * W5500_OK when that is done, W5500_FULL when the chip has no room or, with two sockets, the
 * next socket is still sending (the datagram is held and goes first next time), or the driver
 * error, after which nothing more is sent. Call w5500_udp_poll on each socket when the queue is
 * empty, so the last datagrams' SENDs start and finish. */
int w5500_ts_send(w5500_ts_sender *t, bool partial);
/* Two sockets only: another datagram (n bytes from buf, which follows w5500_udp_send's DMA rule)
 * in the sockets' turn, to port on the sockets' destination address, which is then set back. In
 * the turn, the chip still holds at most one SEND behind the frame on the wire, as for TS; a
 * third socket let the W5500 lose whole TS datagrams. W5500_OK, W5500_FULL (the socket in turn
 * is still sending, or a TS datagram is held: try later) or an error. */
int w5500_ts_send_other(w5500_ts_sender *t, const uint8_t *buf, size_t n, uint16_t port);

#endif
