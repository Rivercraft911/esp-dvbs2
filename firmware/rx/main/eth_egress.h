#pragma once

#include <stdint.h>
#include "ts_egress.h"

/* `rx ... ts eth`: the transport stream out through a W5500 (components/w5500) as UDP datagrams
 * of seven TS packets to the eth settings' destination (sdkconfig.defaults: the laptop,
 * 192.168.77.1:1234).
 *
 * The checker task copies each packet into a queue of 256 in PSRAM and never waits: a packet
 * that finds it full is dropped and counted. A sender task on core 0 drains it, priority 10:
 * below the checker (11), above the decoder (5), whose repairs of failed frames take
 * milliseconds. It wakes after each frame (ts_egress_t.flush), sends every full datagram, and
 * sends what is left as a shorter datagram once it has waited 10 ms, or at the end. Everything
 * is static or made at boot: no heap after boot. */

typedef struct {
    int status;                     /* w5500.h result of the start, or the error that stopped it */
    int phy;                        /* PHYCFGR at the start (w5500.h W5500_PHY_*) or an error */
    uint32_t datagrams, packets;    /* handed to the chip */
    uint32_t partial;               /* datagrams of fewer than seven packets */
    uint32_t sent;                  /* SEND finished (SEND_OK) */
    uint32_t full;                  /* sends the chip refused for lack of room, retried */
    uint32_t arp_dropped;           /* datagrams lost to a SEND TIMEOUT (ARP) */
    uint32_t dropped, peak, queue;  /* packets the queue refused, most waiting, its size */
    uint32_t max_send_cycles;       /* slowest w5500_udp_send, wall time in CPU cycles */
    uint64_t send_cycles;
    uint32_t stack_free;            /* the sender's least free stack, bytes */
} eth_egress_stats_t;

/* At boot, before arena_booted: the SPI port and the sender task. */
void eth_egress_init(void);
/* Before a run: start the chip and open the socket. The egress for receive_options_t, or NULL
 * if the chip did not start (stats->status says why). */
const ts_egress_t *eth_egress_begin(eth_egress_stats_t *stats);
/* After the run, once the checker has stopped: send what is left, close the socket, count. */
void eth_egress_end(eth_egress_stats_t *stats);
/* The summary's "eth" member, after a comma. */
void eth_egress_print(const eth_egress_stats_t *stats);
