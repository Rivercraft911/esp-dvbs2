#pragma once

#include <stdint.h>
#include "capture.h"
#include "monitor.h"
#include "receive.h"
#include "ts_egress.h"
#include "w5500.h"

/* `rx ... ts eth`: the transport stream out through a W5500 (components/w5500) as UDP datagrams
 * of seven TS packets to the eth settings' destination (sdkconfig.defaults: the laptop,
 * 192.168.77.1:1234).
 *
 * The checker task copies each packet into a queue of 4,096 in PSRAM (0.68 s at 8.8 Mb/s, for
 * the bursts of a camera mux) and never waits: a packet that finds it full is dropped and
 * counted. A sender task on core 0 drains it at priority 13: above the checker (11), which keeps
 * core 0 busy for long stretches at 8PSK 5 Msym/s (below it the egress topped out near 6.9 Mb/s),
 * and below the release (14), whose ring core 1 fills (above it, a saturated egress cost the
 * demodulator 356 to 484 runs in 20 s). It sends on sockets 0 and 1 in turn (w5500_ts.h), so the
 * chip holds the next datagram while one is on the wire. It wakes after each frame
 * (ts_egress_t.flush) and, by an esp_timer, when the socket it waits for should be free; sends
 * every full datagram, and sends what is left as a shorter datagram once it has waited 10 ms, or
 * at the end. Everything is static or made at boot: no heap after boot. */

typedef struct {
    int status;                     /* w5500.h result of the start, or the error that stopped it */
    int phy;                        /* PHYCFGR at the start (w5500.h W5500_PHY_*) or an error */
    uint32_t datagrams, packets;    /* handed to the chip */
    uint32_t partial;               /* datagrams of fewer than seven packets */
    uint32_t sent;                  /* SEND finished (SEND_OK) */
    uint32_t resends;               /* SENDs the chip sat on for 20 ms, given again */
    uint32_t full;                  /* sends the chip refused for lack of room, retried */
    uint32_t arp_dropped;           /* datagrams lost to a SEND TIMEOUT (ARP) */
    uint32_t dropped, peak, queue;  /* packets the queue refused, most waiting, its size */
    uint32_t max_send_cycles;       /* slowest w5500_udp_send, wall time in CPU cycles */
    uint64_t send_cycles;
    uint32_t stack_free;            /* the sender's least free stack, bytes */
    int esp_err;                    /* the ESP-IDF error behind a "spi" status */
    uint32_t underflows;            /* payload writes the SPI DMA underran, written again */
    uint32_t recoveries;            /* ... ten times in a row: GP-SPI2 and the GDMA reset */
    /* The monitor stream (mon_pack.h), in the TS sockets' turn: datagrams sent and dropped (not
     * sent within their period, TS first), sample snapshots, and the wall time per datagram (its sends
     * sleep while the payload's DMA runs). */
    bool mon;
    uint32_t mon_sent, mon_dropped, mon_max_cycles;
    uint32_t mon_copies, mon_copy_max;      /* sample snapshots taken, and refused */
    uint64_t mon_cycles;
    int64_t backlog_us;             /* time with two datagrams' worth or more waiting */
    uint32_t backlog_datagrams;     /* sent meanwhile: their rate is the sender's drain rate */
    /* When the run ended badly: the chip's socket registers and the driver's view of them. */
    bool stuck;
    w5500_regs regs;
    uint16_t flight, staged, tx_wr, tx_end;
    bool cmd_pending;
} eth_egress_stats_t;

/* At boot, before arena_booted: the SPI port and the sender task. */
void eth_egress_init(void);
/* What the monitor stream reports about the run, for eth_egress_begin. */
typedef struct {
    unsigned mhz, gain, modcod, rate;
    const receive_stats_t *rx;      /* live counters, as the receiver keeps them */
    const capture_stats_t *capture;
} eth_run_t;

/* Before a run: start the chip and open the sockets. The egress for receive_options_t, or NULL
 * if the chip did not start (stats->status says why). With the monitor on (eth mon), *mon is
 * its hooks for receive_options_t, else NULL. */
const ts_egress_t *eth_egress_begin(eth_egress_stats_t *stats, const eth_run_t *run,
                                    rx_monitor_t **mon);
/* After the run, once the checker has stopped: send what is left, close the socket, count. */
void eth_egress_end(eth_egress_stats_t *stats);
/* The monitor stream's rates per second, 0 to 10 each (0: off): sample snapshots, symbol taps
 * and stats records. Default 10, 10, 5. */
void eth_egress_monitor_rates(unsigned samples, unsigned symbols, unsigned stats);
/* The summary's "eth" member, after a comma. */
void eth_egress_print(const eth_egress_stats_t *stats);
