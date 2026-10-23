/* W5500 UDP sockets: registers, pointer arithmetic and the send/receive sequences (w5500.h).
 * Portable C99; the bus callbacks do the SPI. Register offsets cite the W5500 Datasheet v1.1.0. */
#include "w5500.h"

#include <string.h>

/* Common register block, BSB 0 (DS Table 3, p.29). */
#define MR 0x0000          /* DS p.32: bit 7 RST, a software reset that clears itself */
#define MR_RST 0x80
#define GAR 0x0001         /* DS p.33: GAR 0x01-04, SUBR 0x05-08, SHAR 0x09-0E, SIPR 0x0F-12 */
#define PHYCFGR 0x002E     /* DS p.42 */
#define VERSIONR 0x0039    /* DS p.43 */

/* Socket n register block, BSB 4n+1 (DS Table 4, p.30; DS 4.2, pp.44-57). */
#define SN_MR 0x00         /* DS pp.44-45: P[3:0] = 0010 is UDP */
#define SN_CR 0x01         /* DS pp.46-48; reads 0 once the chip has accepted a command */
#define SN_IR 0x02         /* DS p.48: write 1 to clear a bit */
#define SN_SR 0x03         /* DS pp.49-50 */
#define SN_PORT 0x04       /* DS p.51 */
#define SN_DIPR 0x0C       /* DS p.51; Sn_DPORT follows at 0x10 (DS pp.51-52) */
#define SN_RXBUF_SIZE 0x1E /* DS pp.52-53; Sn_TXBUF_SIZE follows at 0x1F (DS p.53) */
#define SN_TX_RD 0x22      /* DS p.54; then Sn_TX_WR 0x24, Sn_RX_RSR 0x26, Sn_RX_RD 0x28 */
#define SN_TX_WR 0x24      /* DS pp.54-55 */
#define SN_RX_RD 0x28      /* DS p.55 */
#define SN_RX_WR 0x2A      /* DS pp.55-56 */

#define MR_UDP 0x02
#define CR_OPEN 0x01
#define CR_CLOSE 0x10
#define CR_SEND 0x20
#define CR_RECV 0x40
#define IR_SEND_OK 0x10
#define IR_TIMEOUT 0x08
#define IR_ALL 0x1F        /* bits 7-5 are reserved (DS p.48) */
#define SOCK_CLOSED 0x00
#define SOCK_UDP 0x22

#define BSB_COMMON 0u
#define BSB_REG(s) (4u * (s) + 1u)
#define BSB_TX(s) (4u * (s) + 2u)
#define BSB_RX(s) (4u * (s) + 3u)

#define RESET_POLLS 100    /* x 100 us */
#define COMMAND_POLLS 100  /* x 10 us: Sn_CR acceptance */
#define STATE_POLLS 100    /* x 100 us: Sn_SR after OPEN or CLOSE */
#define SETTLE_POLLS 6     /* x 25 us: longer than a 1,500-byte frame on a 100 Mb/s wire */

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static int frame(w5500 *w, unsigned bsb, uint16_t offset, const uint8_t *tx, uint8_t *rx, size_t n,
                 bool post)
{
    /* DS 2.2.2: BSB[4:0] in bits 7-3, RWB (1 = write) in bit 2, OM = 00. */
    const uint8_t control = (uint8_t)(bsb << 3 | (tx ? 4u : 0u));
    w->frames++;
    w->frame_bytes += (uint32_t)(3 + n);
    if (w->bus.frame(w->bus.ctx, offset, control, tx, rx, n, post)) {
        w->bus_errors++;
        return W5500_ERR_BUS;
    }
    return W5500_OK;
}

static int rd(w5500 *w, unsigned bsb, uint16_t offset, uint8_t *p, size_t n)
{
    return frame(w, bsb, offset, NULL, p, n, false);
}

static int wr(w5500 *w, unsigned bsb, uint16_t offset, const uint8_t *p, size_t n)
{
    return frame(w, bsb, offset, p, NULL, n, false);
}

static int wr16(w5500 *w, unsigned s, uint16_t offset, uint16_t v)
{
    const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return wr(w, BSB_REG(s), offset, b, 2);
}

static bool size_ok(unsigned kb)
{
    return kb <= 16 && !(kb & (kb - 1));   /* 0, 1, 2, 4, 8 or 16 (DS pp.52-53) */
}

int w5500_init(w5500 *w, const w5500_bus *bus, const w5500_config *c)
{
    memset(w, 0, sizeof *w);
    w->bus = *bus;
    unsigned tx = 0, rx = 0;
    for (unsigned s = 0; s < W5500_SOCKETS; ++s) {
        if (!size_ok(c->tx_kb[s]) || !size_ok(c->rx_kb[s])) return W5500_ERR_ARG;
        tx += c->tx_kb[s];
        rx += c->rx_kb[s];
    }
    if (tx > 16 || rx > 16) return W5500_ERR_ARG;

    int e = rd(w, BSB_COMMON, VERSIONR, &w->version, 1);
    if (e) return e;
    if (w->version != W5500_VERSION) return W5500_ERR_CHIP;   /* MISO, SCLK or CS fault */
    const uint8_t reset = MR_RST;
    if ((e = wr(w, BSB_COMMON, MR, &reset, 1))) return e;
    for (unsigned i = 0;; ++i) {
        uint8_t mr;
        if ((e = rd(w, BSB_COMMON, MR, &mr, 1))) return e;
        if (!(mr & MR_RST)) break;
        if (i == RESET_POLLS) return W5500_ERR_CHIP;
        w->bus.delay_us(w->bus.ctx, 100);
    }

    /* GAR, SUBR, SHAR and SIPR are one 18-byte run (DS p.29, p.33). Reading it back proves the
     * MOSI path, which VERSIONR cannot. */
    uint8_t net[18], back[18];
    memcpy(net, c->gateway, 4);
    memcpy(net + 4, c->mask, 4);
    memcpy(net + 8, c->mac, 6);
    memcpy(net + 14, c->ip, 4);
    if ((e = wr(w, BSB_COMMON, GAR, net, sizeof net)) ||
        (e = rd(w, BSB_COMMON, GAR, back, sizeof back))) return e;
    if (memcmp(net, back, sizeof net)) return W5500_ERR_CHIP;

    /* Size all eight sockets, so the unused ones release their 2 KB defaults (DS p.31). */
    for (unsigned s = 0; s < 8; ++s) {
        const bool used = s < W5500_SOCKETS;
        const uint8_t kb[2] = {used ? c->rx_kb[s] : 0, used ? c->tx_kb[s] : 0};
        if ((e = wr(w, BSB_REG(s), SN_RXBUF_SIZE, kb, 2))) return e;
        if (used) {
            w->sock[s].rx_size = (uint16_t)(kb[0] * 1024u);
            w->sock[s].tx_size = (uint16_t)(kb[1] * 1024u);
        }
    }
    return W5500_OK;
}

int w5500_phy(w5500 *w)
{
    uint8_t v;
    const int e = rd(w, BSB_COMMON, PHYCFGR, &v, 1);
    return e ? e : v;
}

/* The chip clears Sn_CR once it has accepted a command (DS p.46); a command written before that
 * may be lost. Acceptance is not completion, which shows in Sn_SR or Sn_IR. Each command marks
 * the socket, and the next one first reads Sn_CR until it is 0 (usually once, as the data path
 * puts several frames between two commands). */
static int accepted(w5500 *w, unsigned s)
{
    w5500_socket *k = &w->sock[s];
    for (unsigned i = 0; k->cmd_pending; ++i) {
        uint8_t cr;
        const int e = rd(w, BSB_REG(s), SN_CR, &cr, 1);
        if (e) return e;
        if (!cr) k->cmd_pending = false;
        else if (i == COMMAND_POLLS) return W5500_ERR_CHIP;
        else w->bus.delay_us(w->bus.ctx, 10);
    }
    return W5500_OK;
}

static int command(w5500 *w, unsigned s, uint8_t cmd)
{
    int e = accepted(w, s);
    if (!e) e = wr(w, BSB_REG(s), SN_CR, &cmd, 1);
    if (!e) w->sock[s].cmd_pending = true;
    return e;
}

/* Wait for Sn_SR to show that OPEN or CLOSE has completed (DS pp.46-49). */
static int state(w5500 *w, unsigned s, uint8_t want)
{
    for (unsigned i = 0;; ++i) {
        uint8_t sr;
        const int e = rd(w, BSB_REG(s), SN_SR, &sr, 1);
        if (e) return e;
        if (sr == want) return W5500_OK;
        if (i == STATE_POLLS) return W5500_ERR_SOCKET;
        w->bus.delay_us(w->bus.ctx, 100);
    }
}

/* Close (unless closed), set UDP mode and the port, open, and take the pointers OPEN initialized
 * (DS p.54-55). Keeps statistics and the destination, drops anything staged. */
static int open_socket(w5500 *w, unsigned s)
{
    w5500_socket *k = &w->sock[s];
    const uint8_t mode = MR_UDP, clear = IR_ALL;
    uint8_t p[8];
    k->open = false;
    int e = rd(w, BSB_REG(s), SN_SR, p, 1);
    if (!e && p[0] != SOCK_CLOSED) e = command(w, s, CR_CLOSE);
    if (!e) e = state(w, s, SOCK_CLOSED);
    if (!e) e = wr(w, BSB_REG(s), SN_MR, &mode, 1);
    if (!e) e = wr16(w, s, SN_PORT, k->port);
    if (!e) e = wr(w, BSB_REG(s), SN_IR, &clear, 1);
    if (!e) e = command(w, s, CR_OPEN);
    if (!e) e = state(w, s, SOCK_UDP);
    if (!e) e = rd(w, BSB_REG(s), SN_TX_RD, p, 8);   /* TX_RD, TX_WR, RX_RSR, RX_RD */
    if (e) return e;
    k->tx_wr = k->tx_end = get16(p + 2);
    k->rx_rd = get16(p + 6);
    k->flight = 0;
    k->q_head = k->q_count = 0;
    k->rx_left = 0;
    k->rx_dirty = false;
    k->stalled = false;
    k->dest_dirty = true;
    k->open = true;
    return W5500_OK;
}

int w5500_udp_open(w5500 *w, unsigned s, uint16_t port)
{
    if (s >= W5500_SOCKETS) return W5500_ERR_ARG;
    w5500_socket *k = &w->sock[s];
    k->port = port;
    k->rx_last = 0;
    memset(&k->st, 0, sizeof k->st);
    return open_socket(w, s);
}

int w5500_udp_close(w5500 *w, unsigned s)
{
    if (s >= W5500_SOCKETS) return W5500_ERR_ARG;
    w->sock[s].open = false;
    const int e = command(w, s, CR_CLOSE);
    return e ? e : state(w, s, SOCK_CLOSED);
}

void w5500_udp_dest(w5500 *w, unsigned s, const uint8_t ip[4], uint16_t port)
{
    if (s >= W5500_SOCKETS) return;
    memcpy(w->sock[s].dest, ip, 4);
    w->sock[s].dest_port = port;
    w->sock[s].dest_dirty = true;
}

const w5500_stats *w5500_udp_stats(const w5500 *w, unsigned s)
{
    return s < W5500_SOCKETS ? &w->sock[s].st : NULL;
}

/* TX buffer bytes free for staging: Sn_TX_FSR (Sn_TX_WR - Sn_TX_RD, DS pp.53-54) less the staged
 * datagrams. Sn_TX_RD = tx_wr - flight, since a finished SEND leaves Sn_TX_RD = Sn_TX_WR. */
static unsigned tx_room(const w5500_socket *k)
{
    const uint16_t tx_rd = (uint16_t)(k->tx_wr - k->flight);
    return k->tx_size - (uint16_t)(k->tx_end - tx_rd);
}

/* Retire a finished SEND, then give the oldest staged datagram to the chip. */
static int kick(w5500 *w, unsigned s)
{
    w5500_socket *k = &w->sock[s];
    int e;
    if (k->flight) {
        uint8_t r[3];   /* Sn_CR, Sn_IR, Sn_SR */
        if ((e = rd(w, BSB_REG(s), SN_CR, r, 3))) return e;
        if (!r[0]) k->cmd_pending = false;   /* the SEND was accepted */
        if (r[2] != SOCK_UDP) {
            k->open = false;
            return W5500_ERR_SOCKET;
        }
        if (r[0] || !(r[1] & (IR_SEND_OK | IR_TIMEOUT))) return W5500_OK;   /* on the wire */
        if (!(r[1] & IR_SEND_OK)) {
            /* ARP failed (DS p.38, p.48). Where that leaves Sn_TX_RD is not documented, so
             * reopen the socket; the staged datagrams are lost with this one. */
            k->st.dropped += 1u + k->q_count;
            return open_socket(w, s);
        }
        k->st.sent++;
        k->st.sent_bytes += k->flight;
        k->flight = 0;
        /* Clear SEND_OK (write 1, DS p.48) before the next SEND can set it again. */
        const uint8_t ok = IR_SEND_OK;
        if ((e = wr(w, BSB_REG(s), SN_IR, &ok, 1))) return e;
    }
    if (!k->q_count) return W5500_OK;
    if (k->dest_dirty) {
        const uint8_t d[6] = {k->dest[0], k->dest[1], k->dest[2], k->dest[3],
                              (uint8_t)(k->dest_port >> 8), (uint8_t)k->dest_port};
        if ((e = wr(w, BSB_REG(s), SN_DIPR, d, sizeof d))) return e;
        k->dest_dirty = false;
    }
    const uint16_t n = k->queue[k->q_head];
    if ((e = wr16(w, s, SN_TX_WR, (uint16_t)(k->tx_wr + n))) || (e = command(w, s, CR_SEND)))
        return e;
    k->tx_wr = (uint16_t)(k->tx_wr + n);
    k->flight = n;
    k->q_head = (uint8_t)((k->q_head + 1) % W5500_TXQ);
    k->q_count--;
    return W5500_OK;
}

bool w5500_busy(const w5500 *w)
{
    return w->bus.busy && w->bus.busy(w->bus.ctx);
}

int w5500_udp_send(w5500 *w, unsigned s, const uint8_t *data, size_t len)
{
    if (s >= W5500_SOCKETS) return W5500_ERR_ARG;
    w5500_socket *k = &w->sock[s];
    if (!k->open || !len || len > W5500_UDP_MAX || len > k->tx_size) return W5500_ERR_ARG;
    /* A posting backend leaves this payload clocking out when we return, so the SEND for it is
     * started by the next call, here first; a blocking backend starts it right after the
     * payload. Either way one SEND is on the wire while the next payload is written. */
    const bool posting = w->bus.busy != NULL;
    int e;
    if (posting && (e = kick(w, s))) return e;
    if (k->q_count == W5500_TXQ || tx_room(k) < len) {
        if (!posting && (e = kick(w, s))) return e;   /* a finished SEND frees its bytes */
        if (k->q_count == W5500_TXQ || tx_room(k) < len) {
            k->st.full++;
            return W5500_FULL;
        }
    }
    /* Stage past the queued data while the chip may still be sending; one frame even across
     * the end of the buffer, since the chip maps the offset modulo the buffer size. */
    if ((e = frame(w, BSB_TX(s), k->tx_end, data, NULL, len, posting))) return e;
    k->queue[(k->q_head + k->q_count) % W5500_TXQ] = (uint16_t)len;
    k->q_count++;
    k->tx_end = (uint16_t)(k->tx_end + len);
    return posting ? W5500_OK : kick(w, s);
}

int w5500_udp_poll(w5500 *w, unsigned s)
{
    if (s >= W5500_SOCKETS || !w->sock[s].open) return W5500_ERR_ARG;
    const int e = kick(w, s);
    return e ? e : w->sock[s].q_count + (w->sock[s].flight != 0);
}

/* Write Sn_RX_RD and command RECV, releasing what has been read (DS p.55). The previous RECV
 * must have been accepted before Sn_RX_RD changes again. */
static int commit(w5500 *w, unsigned s)
{
    w5500_socket *k = &w->sock[s];
    int e = accepted(w, s);
    if (!e) e = wr16(w, s, SN_RX_RD, k->rx_rd);
    if (!e) e = command(w, s, CR_RECV);
    if (!e) k->rx_dirty = false;
    return e;
}

/* Drop everything known to be received. */
static void flush(w5500_socket *k)
{
    k->rx_rd = (uint16_t)(k->rx_rd + k->rx_left);
    k->rx_left = 0;
    k->rx_dirty = true;
    k->st.malformed++;
}

/* rx_left = Sn_RX_WR - rx_rd. Sn_RX_WR only grows while we read and its MSB comes first, so a
 * single read that straddles an update is low, or wraps below rx_rd and exceeds the buffer size.
 * With `stable`, read until two consecutive reads agree (DS p.55 advice for the size registers).
 * Returns 1 when rx_left was set, 0 when no trustworthy value was found (try again later), or an
 * error. */
static int level(w5500 *w, unsigned s, bool stable)
{
    w5500_socket *k = &w->sock[s];
    uint8_t a[2], b[2];
    int e = rd(w, BSB_REG(s), SN_RX_WR, a, 2);
    if (e) return e;
    if (!stable && (uint16_t)(get16(a) - k->rx_rd) > k->rx_size) stable = true;
    for (unsigned i = 0; stable; ++i) {
        if (i == 4) return 0;
        if ((e = rd(w, BSB_REG(s), SN_RX_WR, b, 2))) return e;
        if (!memcmp(a, b, 2)) break;
        memcpy(a, b, 2);
    }
    const uint16_t n = (uint16_t)(get16(a) - k->rx_rd);
    k->rx_left = n;
    if (n > k->rx_size) {
        flush(k);   /* stable and impossible: our pointer is wrong (a chip reset); resync */
        return 0;
    }
    return 1;
}

/* Sn_RX_WR covers fewer than `need` bytes of the datagram at rx_rd: a straddling read, or the
 * chip publishing a datagram in parts (the datasheet neither rules it out nor bounds how long it
 * may take). Look again every 25 us for up to 150 us, longer than a 1,500-byte frame takes on a
 * 100 Mb/s wire, but only once per position of Sn_RX_WR. Returns 1 once the datagram is covered;
 * otherwise 0 and a later call looks again (counted in `deferred`). Nothing is flushed here: a
 * short record is not proof of corruption. */
static int settle(w5500 *w, unsigned s, unsigned need)
{
    w5500_socket *k = &w->sock[s];
    int r = 0;
    for (unsigned i = 0; i < SETTLE_POLLS; ++i) {
        if (i) w->bus.delay_us(w->bus.ctx, 25);
        if ((r = level(w, s, true)) < 0) return r;
        if (!k->rx_left) return 0;                       /* resynchronized by level() */
        if (!r) continue;                                /* Sn_RX_WR still moving */
        if (k->rx_left >= need) {
            k->stalled = false;
            return 1;
        }
        if (k->stalled && (uint16_t)(k->rx_rd + k->rx_left) == k->stall_wr) break;
    }
    if (r) {
        k->stalled = true;
        k->stall_wr = (uint16_t)(k->rx_rd + k->rx_left);
    }
    k->st.deferred++;
    return 0;
}

int w5500_udp_recv(w5500 *w, unsigned s, uint8_t *buf, size_t cap, w5500_peer *from)
{
    if (s >= W5500_SOCKETS) return W5500_ERR_ARG;
    w5500_socket *k = &w->sock[s];
    if (!k->open || !k->rx_size) return W5500_ERR_ARG;
    int e, got = 0;
    if (!k->rx_left) {
        if ((e = level(w, s, false)) < 0) return e;
        const unsigned next = W5500_UDP_HEADER + (k->rx_last ? k->rx_last : W5500_UDP_MAX);
        if (e && (unsigned)(k->rx_size - k->rx_left) < next) k->st.overruns++;
    }
    while (k->rx_left) {
        if (k->rx_left < W5500_UDP_HEADER && (e = settle(w, s, W5500_UDP_HEADER)) <= 0) {
            if (e < 0) return e;
            break;
        }
        if ((e = rd(w, BSB_RX(s), k->rx_rd, w->hdr, W5500_UDP_HEADER))) return e;
        const unsigned n = get16(w->hdr + 6), whole = W5500_UDP_HEADER + n;
        if (n > W5500_UDP_MAX || whole > k->rx_size) {   /* no such datagram can arrive */
            flush(k);
            break;
        }
        if (whole > k->rx_left && (e = settle(w, s, whole)) <= 0) {
            if (e < 0) return e;
            break;
        }
        if (!n || n > cap) {
            k->rx_rd = (uint16_t)(k->rx_rd + whole);
            k->rx_left = (uint16_t)(k->rx_left - whole);
            k->rx_dirty = true;
            k->st.discarded++;
            continue;
        }
        if ((e = rd(w, BSB_RX(s), (uint16_t)(k->rx_rd + W5500_UDP_HEADER), buf, n))) return e;
        if (from) {
            memcpy(from->ip, w->hdr, 4);
            from->port = get16(w->hdr + 4);
        }
        k->rx_rd = (uint16_t)(k->rx_rd + whole);
        k->rx_left = (uint16_t)(k->rx_left - whole);
        k->rx_dirty = true;
        k->rx_last = (uint16_t)n;
        k->st.received++;
        k->st.received_bytes += n;
        got = (int)n;
        break;
    }
    if (!k->rx_left && k->rx_dirty && (e = commit(w, s))) return e;
    return got;
}
