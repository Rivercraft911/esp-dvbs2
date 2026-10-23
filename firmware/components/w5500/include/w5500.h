#ifndef W5500_H
#define W5500_H

/* WIZnet W5500 hardware UDP sockets over SPI: live MPEG-TS between a laptop and the ESP32-S3s.
 * Portable C99, so the register and pointer logic runs on the host against tests/w5500_fake.c;
 * w5500_eth.c is the ESP-IDF backend (SPI master, MAC, settings in NVS, `eth` console).
 *
 * The chip runs its own TCP/IP stack. A socket in UDP mode sends and receives whole datagrams
 * from 16 KB of TX and 16 KB of RX buffer memory inside the chip, so the S3 needs no lwIP and
 * almost no RAM: the driver state is about 250 bytes and payloads move by SPI DMA straight
 * between the chip and the caller's buffer.
 *
 * Sources. "DS p.N" is the WIZnet W5500 Datasheet Version 1.1.0 (17 Dec 2022). The 8-byte header
 * in front of each received UDP datagram is not in the datasheet; WIZnet's "W5500 UDP Function"
 * page (docs.wiznet.io/Product/Chip/Ethernet/W5500/Application/udp, read 2026-10-22) lists its
 * fields: peer IP (4 bytes), peer port (2), payload length (2). Like every multi-byte W5500
 * field they are read MSB first; the page gives positions only, so the board confirms the order.
 *
 * SPI frame (DS 2.2, pp.14-17). Mode 0, MSB first (DS p.12); CS low for the whole frame
 * (variable length data mode, DS 2.3 p.17):
 *   bytes 0-1  16-bit offset address (DS 2.2.1, p.14)
 *   byte 2     control: BSB[4:0] << 3 | RWB << 2 | OM[1:0] (DS 2.2.2, pp.15-17).
 *              BSB 0 = common registers; for socket n, 4n+1 = its registers, 4n+2 = its TX
 *              buffer, 4n+3 = its RX buffer; 4n+4 is reserved (DS pp.15-16). RWB 1 = write
 *              (DS p.16). OM 00 = variable length data mode, the only mode used here (DS p.17).
 *   bytes 3..  N >= 1 data bytes; the offset increments by one per byte (DS p.14).
 * Example (DS p.20): five bytes to socket 1's TX buffer at 0x0040 are 00 40 34 then the data.
 *
 * Buffers and pointers. Each socket's TX and RX buffer is 0, 1, 2, 4, 8 or 16 KB; the sizes of
 * all eight sockets must each sum to at most 16 KB and are allocated in socket order
 * (DS 3.3 p.31, Sn_RXBUF_SIZE/Sn_TXBUF_SIZE pp.52-53). A buffer is addressed with a 16-bit offset
 * that the chip maps onto the buffer modulo its size (DS p.31, p.53), and the pointers
 * Sn_TX_RD/WR and Sn_RX_RD/WR are free-running 16-bit counters that drop the carry
 * (DS pp.54-56). So the driver keeps 16-bit copies of the pointers, writes and reads at those
 * offsets, and never splits a burst at the end of a buffer.
 *
 * Commands (DS Sn_CR pp.46-48). The chip clears Sn_CR when it has accepted a command, and a
 * command written before that may be lost, so every command first waits for Sn_CR = 0 (one read,
 * or none when a status read already showed it). Acceptance is not completion: OPEN and CLOSE
 * wait for Sn_SR to reach SOCK_UDP or SOCK_CLOSED (DS pp.49-50).
 *
 * Send (DS Sn_TX_WR pp.54-55, Sn_CR SEND p.47). Write the datagram at Sn_TX_WR, advance
 * Sn_TX_WR, command SEND; SEND transmits Sn_TX_RD..Sn_TX_WR as one datagram and sets SEND_OK in
 * Sn_IR when done (DS p.48), or TIMEOUT if ARP for a unicast destination fails (DS p.38). The
 * driver clears SEND_OK before the next SEND, so a completion is never lost.
 * Only one SEND may be outstanding, so the driver stages the next datagrams in the TX buffer
 * past Sn_TX_WR while a SEND is on the wire and starts each as the previous one completes.
 * The payload burst of datagram k+1 (at least 264 us of SPI, at 40 MHz) outlasts the wire time of
 * datagram k (110.6 us at 100 Mb/s, below), so udp_send normally finds the last SEND finished
 * and never waits; the 16 KB buffer then absorbs ARP waits and link stalls (12 datagrams of
 * 1,316 bytes, about 10 ms at 12.64 Mb/s). udp_send returns W5500_FULL instead of blocking when
 * the buffer has no room; the room is what Sn_TX_FSR would report (DS pp.53-54), computed from
 * the driver's own pointer copies instead of read.
 *
 * Receive (DS Sn_RX_RD p.55, Sn_RX_WR pp.55-56, Sn_CR RECV p.48). The chip stores each datagram
 * as the 8-byte header and the payload at Sn_RX_WR. The driver reads Sn_RX_WR (not Sn_RX_RSR,
 * which depends on when RECV is processed), then the header and payload at its own read
 * pointer, and finally writes Sn_RX_RD and commands RECV once everything known has been read
 * and the previous RECV was accepted. Sn_RX_WR only grows while the driver reads, and its MSB is
 * read first, so a read that straddles an update is never high. When Sn_RX_WR covers only part of
 * the next datagram (a straddling read, or a chip that publishes a datagram in parts, which the
 * datasheet does not rule out), the driver re-reads it until two reads agree (DS p.55 advice for
 * the size registers) and waits up to 150 us for the rest, once per position of Sn_RX_WR; if it is
 * still short, the call returns 0 and a later call looks again (`deferred`). Only what no chip
 * state can explain is flushed as malformed: a length over 1,472 bytes or the buffer, or Sn_RX_WR
 * outside the buffer. A record that never completes is left to the caller, who can reopen the
 * socket.
 *
 * Throughput (derived; the board measures it, `eth send` and `eth recv`). Each access is one SPI
 * frame: 3 header bytes plus data. The ESP backend polls frames of at most W5500_SMALL_MAX data
 * bytes and runs longer ones as interrupt DMA transactions. ESP-IDF v6.1 quotes, for the
 * ESP32-S3, t_p = 11 us per polled DMA transaction and t_q = 26 us per interrupt DMA transaction
 * of one byte (docs/en/api-reference/peripherals/spi_master.rst, "Transaction Duration"); they are
 * used here as fixed costs per frame, with the bits added at f_SCK.
 *   Send, one 1,316-byte datagram (7 TS packets), steady state:
 *     payload write 3+1316, Sn_CR..Sn_SR read 3+3, SEND_OK clear 3+1, Sn_TX_WR write 3+2,
 *     SEND 3+1 = 1 burst + 4 polled frames, 1,338 bytes = 10,704 bits
 *     T_send = t_q + 4 t_p + 10,704 / f_SCK
 *   Receive, one datagram, nothing else queued:
 *     Sn_RX_WR read 3+2, header read 3+8, payload read 3+1316, Sn_CR read (last RECV accepted)
 *     3+1, Sn_RX_RD write 3+2, RECV 3+1 = 1 burst + 5 polled frames, 1,348 bytes = 10,784 bits
 *     T_recv = t_q + 5 t_p + 10,784 / f_SCK
 *   Payload rate R = 10,528 bits / T (1,316 bytes x 8):
 *     f_SCK (80 MHz/n)   T_send    R_send      T_recv    R_recv
 *     40.00 MHz (/2)     337.6 us  31.2 Mb/s   350.6 us  30.0 Mb/s
 *     26.67 MHz (/3)     471.4 us  22.3 Mb/s   485.4 us  21.7 Mb/s   default
 *     20.00 MHz (/4)     605.2 us  17.4 Mb/s   620.2 us  17.0 Mb/s
 *     10.00 MHz (/8)    1140.4 us   9.2 Mb/s  1159.4 us   9.1 Mb/s
 *   15 Mb/s needs T <= 701.9 us, so f_SCK >= 16.9 MHz for send and 17.4 MHz for receive: 20 MHz
 *   is the slowest S3 divider that meets the goal. The W5500 is guaranteed to 33.3 MHz and
 *   designed for 80 MHz (DS p.61), so 26.67 MHz is the default and 40 MHz is for trying.
 *   Wire: a 1,316-byte datagram is a 1,362-byte Ethernet frame (14 MAC + 20 IP + 8 UDP + 4 FCS)
 *   plus 20 bytes of preamble and gap: 1,382 bytes = 110.6 us at 100 Mb/s, shorter than the SPI
 *   time, so SPI sets the rate. On a 10 Mb/s link (1.106 ms) the wire caps payload at 9.5 Mb/s,
 *   below the 12.64 Mb/s receiver mode, so the link must be 100 Mb/s (`eth` prints it).
 *   CPU on the calling core: a polled frame busies the core for t_p plus its bits; an interrupt
 *   transaction costs about t_q while DMA moves the payload. Send: 4 x 11 + 19 x 8 / 26.67 +
 *   26 = 75.7 us per datagram; receive: 5 x 11 + 29 x 8 / 26.67 + 26 = 89.7 us. At the 12.64 Mb/s
 *   receiver mode (1,200.6 datagrams/s) that is 9.1% (send) and 10.8% (receive) of one 240 MHz
 *   core, plus 12.5 us for each empty receive poll. Most of it is ESP-IDF's fixed cost per
 *   transaction, so a faster clock barely helps.
 *   Direct GP-SPI2 backend (CONFIG_W5500_DIRECT_SPI; estimated, the board measures): a polled
 *   frame costs about 1.0 us of register work plus its bits; a payload about 1.5 us to set up
 *   its GDMA descriptor and start. A payload write is posted (udp_send returns while it clocks
 *   out); a read sleeps on the SPI interrupt, about 6 us of CPU and 2 us of latency.
 *     send, caller paced:  CPU 4 x 1.0 + 152 / f_SCK + 1.5 = 11.2 us at 26.67 MHz
 *     send, back to back:  1.5 + 10,552 / f_SCK + 2 + 4 x 1.0 + 152 / f_SCK = 408.9 us, 25.7 Mb/s
 *     receive:             CPU 5 x 1.0 + 232 / f_SCK + 1.5 + 6 = 21.2 us; 412.9 us, 25.5 Mb/s
 *     at 40 MHz:           send 9.3 us CPU, 38.3 Mb/s; receive 18.3 us CPU, 37.9 Mb/s
 *   At 12.64 Mb/s that is 1.3% (send) and 2.5% (receive) of one core.
 *   tests/test_w5500.py replays these frame sequences through the fake and checks the frame
 *   counts, bytes and times above for both backends. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define W5500_SOCKETS 2      /* sockets 0 and 1; sockets 2-7 get no buffer memory */
#define W5500_UDP_MAX 1472   /* 1500-byte MTU - 20 IP - 8 UDP: no IP fragmentation (DS p.2) */
#define W5500_UDP_HEADER 8   /* peer IP, peer port, payload length, in front of each datagram */
#define W5500_TXQ 16         /* datagrams staged in the TX buffer, at most */
#define W5500_SMALL_MAX 32   /* frames up to this many data bytes are polled by the backend */
#define W5500_VERSION 0x04   /* VERSIONR (DS p.43) */

/* PHYCFGR status bits (DS p.42). */
#define W5500_PHY_LINK 0x01
#define W5500_PHY_100M 0x02
#define W5500_PHY_FULL 0x04
/* PHYCFGR OPMD and OPMDC (DS p.42) for w5500_phy_config. AUTO leaves the mode to the PMODE pins
 * (OPMD 0) with OPMDC "all capable, auto-negotiation"; the others force one mode (OPMD 1) without
 * auto-negotiation. */
#define W5500_PHY_AUTO 0x38
#define W5500_PHY_100F 0x58
#define W5500_PHY_100H 0x50
#define W5500_PHY_10F 0x48
#define W5500_PHY_10H 0x40

enum {
    W5500_OK = 0,
    W5500_FULL = 1,        /* udp_send: no room now; try again later */
    W5500_ERR_ARG = -1,
    W5500_ERR_BUS = -2,    /* the backend failed a frame */
    W5500_ERR_CHIP = -3,   /* wrong VERSIONR, reset or command not finishing, register write lost */
    W5500_ERR_SOCKET = -4, /* the socket did not open, or the chip closed it */
    W5500_ERR_BUSY = -5,   /* w5500_eth: another task owns the chip */
};

typedef struct {
    /* One SPI frame: the 3-byte header, then n >= 1 data bytes written from tx or read into rx
     * (exactly one is non-NULL). Frames longer than W5500_SMALL_MAX carry a datagram payload and
     * point straight into the caller's buffer. With post (only for a payload write) the backend
     * may return while the bytes are still being read from tx; it finishes them before it starts
     * any other frame. Return 0 on success. */
    int (*frame)(void *ctx, uint16_t offset, uint8_t control, const uint8_t *tx, uint8_t *rx,
                 size_t n, bool post);
    void (*delay_us)(void *ctx, uint32_t us);
    /* Optional: true while a posted write is still running. Backends without it never post. */
    bool (*busy)(void *ctx);
    void *ctx;
} w5500_bus;

typedef struct {
    uint8_t mac[6], ip[4], mask[4], gateway[4];
    /* KB per socket: 0, 1, 2, 4, 8 or 16; TX and RX each sum to at most 16 (DS pp.52-53). */
    uint8_t tx_kb[W5500_SOCKETS], rx_kb[W5500_SOCKETS];
} w5500_config;

typedef struct {
    uint8_t ip[4];
    uint16_t port;
} w5500_peer;

typedef struct {
    uint32_t sent, sent_bytes;          /* datagrams whose SEND finished with SEND_OK */
    uint32_t full;                      /* udp_send calls refused for lack of TX room */
    uint32_t dropped;                   /* accepted datagrams lost to a SEND TIMEOUT (ARP) */
    uint32_t received, received_bytes;  /* datagrams delivered by udp_recv */
    uint32_t overruns;   /* Sn_RX_WR reads that found room for less than one more datagram of the
                          * last length: the chip drops datagrams that do not fit, so each marks a
                          * possible loss (TS continuity counters downstream measure real loss) */
    uint32_t discarded;  /* received datagrams skipped: empty or longer than the caller's buffer */
    uint32_t malformed;  /* impossible lengths or Sn_RX_WR outside the buffer: flushed */
    uint32_t deferred;   /* receive calls that found a datagram not yet fully received, or
                          * Sn_RX_WR still moving, and returned 0 to look again later; if it
                          * keeps rising with nothing received, reopen the socket */
} w5500_stats;

typedef struct {
    uint16_t tx_size, rx_size;   /* bytes */
    uint16_t port, dest_port;
    uint8_t dest[4];
    uint16_t tx_wr;              /* Sn_TX_WR as written: the end of the data given to SEND */
    uint16_t tx_end;             /* end of the staged datagrams */
    uint16_t flight;             /* length of the SEND in progress, 0 if none */
    uint16_t queue[W5500_TXQ];   /* staged datagram lengths, oldest at q_head */
    uint8_t q_head, q_count;
    uint16_t rx_rd;              /* read pointer; Sn_RX_RD lags until the next RECV */
    uint16_t rx_left;            /* bytes known received and not yet read */
    uint16_t rx_last;            /* length of the last datagram received */
    uint16_t stall_wr;           /* Sn_RX_WR at the last short look at the next datagram */
    bool open, dest_dirty, rx_dirty, stalled;
    bool cmd_pending;            /* Sn_CR written and not yet seen accepted */
    w5500_stats st;
} w5500_socket;

typedef struct {
    w5500_bus bus;
    w5500_socket sock[W5500_SOCKETS];
    uint32_t frames, frame_bytes, bus_errors;
    uint8_t version;
    uint8_t hdr[W5500_UDP_HEADER];
} w5500;

/* One owner task; nothing here blocks except the reset and open/close command waits.
 * Read VERSIONR, software-reset the chip, set MAC/IP/mask/gateway (read back to check the
 * MOSI path), and size the socket buffers. */
int w5500_init(w5500 *w, const w5500_bus *bus, const w5500_config *cfg);
/* PHYCFGR (DS p.42), whose low bits are W5500_PHY_*, or a negative error. */
int w5500_phy(w5500 *w);
/* Put the PHY in a W5500_PHY_AUTO..10H mode: PHYCFGR written with RST low (the PHY held in reset),
 * then 1 ms later with RST high. PHYCFGR read back, or a negative error. */
int w5500_phy_config(w5500 *w, uint8_t mode);

/* Open socket s in UDP mode on a local port (also the source port of sent datagrams). */
int w5500_udp_open(w5500 *w, unsigned s, uint16_t port);
int w5500_udp_close(w5500 *w, unsigned s);
/* Destination for udp_send; 255.255.255.255 broadcasts without ARP, other addresses are
 * resolved by the chip's ARP on the first SEND. Applied before the next SEND starts. */
void w5500_udp_dest(w5500 *w, unsigned s, const uint8_t ip[4], uint16_t port);
/* Stage one datagram (1..W5500_UDP_MAX bytes, at most the TX buffer) and start it if the chip
 * is idle. W5500_OK, W5500_FULL (nothing written; retry later) or an error. Payload buffers
 * longer than W5500_SMALL_MAX bytes must be DMA capable, 4-byte aligned internal RAM.
 * With a backend that posts (bus.busy set: the direct GP-SPI2 one), udp_send returns while the
 * payload is still clocking out of data, so keep data unchanged until the next w5500 call on
 * this chip returns or w5500_busy() is false; that next call (udp_send or udp_poll) also starts
 * this datagram's SEND. */
int w5500_udp_send(w5500 *w, unsigned s, const uint8_t *data, size_t len);
/* A posted payload write is still running. */
bool w5500_busy(const w5500 *w);
/* Finish a completed SEND and start the next staged datagram. Returns the datagrams still
 * staged or on the wire (0 when all are out), or an error. Call it when idle. */
int w5500_udp_poll(w5500 *w, unsigned s);
/* Copy the next datagram's payload into buf: its length, 0 if none, or an error. Datagrams
 * longer than cap are skipped and counted. buf follows the same DMA rule as udp_send. */
int w5500_udp_recv(w5500 *w, unsigned s, uint8_t *buf, size_t cap, w5500_peer *from);
const w5500_stats *w5500_udp_stats(const w5500 *w, unsigned s);

/* The `eth send` diagnostic (w5500_diag.c). Datagram seq is 7 TS packets on PID 0x100 with
 * running continuity counters, each payload starting with seq, MSB first. */
#define W5500_DIAG_BYTES 1316
void w5500_diag_pattern(uint8_t *p, uint32_t seq);
/* Fill buf[seq & 1] with datagram seq and send it. With a posting backend the buffer sent last
 * may still be clocking out until the next call's first frame, so the two buffers alternate and
 * each is refilled only two sends later, after that send has finished it. */
int w5500_diag_send(w5500 *w, unsigned s, uint8_t *const buf[2], uint32_t seq);

/* Settings kept in NVS by the ESP backend; the console grammar is portable so it is tested on
 * the host (w5500_settings.c). */
typedef struct {
    uint8_t ip[4], mask[4], gateway[4], dest[4];
    uint16_t port, dest_port;   /* local port (listen and source), destination port */
    uint16_t sck_khz;           /* SPI clock request, 10000..40000 */
    int8_t sclk, mosi, miso, cs;
} w5500_settings;

void w5500_settings_default(w5500_settings *s);
/* Pins usable on these boards and distinct, ports nonzero, clock 10-40 MHz. */
bool w5500_settings_valid(const w5500_settings *s);
/* One `eth` setting: "ip A.B.C.D/N [gw A.B.C.D]", "dest A.B.C.D|broadcast[:PORT]",
 * "port PORT", "mhz F" (10..40), "pins SCLK MOSI MISO CS", "defaults".
 * Returns 1 if s changed, -1 on a syntax error (s unchanged). */
int w5500_settings_parse(w5500_settings *s, const char *args);
/* JSON members without braces, e.g. "ip":"192.168.77.2",...; snprintf return value. */
int w5500_settings_json(const w5500_settings *s, char *out, size_t cap);

#endif
