#pragma once

#include <stdint.h>

/* Where the receiver's transport stream goes (`rx ... ts`): an Ethernet sender, or nothing.
 *
 * packet: one 188-byte TS packet from its 0x47 sync byte, valid only during the call. Packets
 *   come in stream order, each checked by the CRC-8 of DVB-S2 mode adaptation: a packet that
 *   failed it has transport_error_indicator (byte 1, bit 7) set, and a packet the deframer could
 *   not check (the last before a lost frame) passes as it came. Null packets (PID 0x1fff) are
 *   left out if the run asked for that (`nonull`).
 * flush: optional, after the last packet of each frame (every 1.1 to 1.7 ms at 5 Msym/s, 5.6 to
 *   8.4 ms at 1 Msym/s), so a datagram sender can send a partial datagram rather than hold it.
 *
 * Both run in the checker task on core 0 (priority 11; the release task, priority 14, preempts
 * it), after the frame's record has gone to USB, and nowhere else. Budget, estimated from the
 * runs without an egress (results/2026-10-23-ts-link): at 8PSK 8/9, 5 Msym/s (12.64 Mb/s,
 * 8,400 packets/s) core 0 is about half idle, which leaves about 40 us per packet (9,600
 * cycles) on average. A call must not block: the checker holds at most three frames, and once
 * they are full the release task waits up to 2 ms for one while the demodulator's 0.82 ms symbol
 * ring fills, so a stall drops frames (frames_dropped) or runs (demod dropped). So an Ethernet
 * egress copies each packet into a bounded queue that its own sender task drains, and counts what
 * it cannot queue; its real cost and the bursts the receiver tolerates are to be measured at
 * bring-up. Internal RAM is short (about 33 KB free during a 5 Msym/s run): keep buffers small or
 * in PSRAM. A packet inside a frame starts 2 bytes past a 4-byte boundary, so a sender that needs
 * aligned DMA buffers (w5500_udp_send) copies it, typically into a datagram of 7 packets
 * (1,316 bytes). */
typedef struct {
    void (*packet)(void *ctx, const uint8_t *packet);
    void (*flush)(void *ctx);
    void *ctx;
} ts_egress_t;
