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
    unsigned sock;
    w5500_ts_queue *q;
    uint8_t *buf[2];                /* W5500_TS_DATAGRAM bytes each, DMA capable (w5500_udp_send) */
    uint32_t (*clock)(void);        /* a cycle counter for the send times, or NULL */
    unsigned next;                  /* the buffer the next datagram is built in */
    uint16_t held;                  /* bytes in buf[next] that the chip refused, 0 if none */
    uint16_t wr;                    /* the socket's Sn_TX_WR after the last SEND seen to start */
    uint32_t wr_at;                 /* clock() just after that call: about when the SEND began */
    w5500_ts_stats st;
} w5500_ts_sender;

/* Consumer side, on an open socket with a destination. The two buffers alternate because a
 * posted payload may still be clocking out of the last one (w5500_udp_send). */
void w5500_ts_sender_init(w5500_ts_sender *t, w5500 *chip, unsigned sock, w5500_ts_queue *q,
                          uint8_t *const buf[2], uint32_t (*clock)(void));
/* Send every full datagram waiting, and with partial the rest as one shorter datagram too.
 * W5500_OK when that is done, W5500_FULL when the chip has no room (the datagram is held and goes
 * first next time), or the driver error, after which nothing more is sent. Call w5500_udp_poll
 * when the queue is empty, so the last datagram's SEND starts and finishes. */
int w5500_ts_send(w5500_ts_sender *t, bool partial);

#endif
