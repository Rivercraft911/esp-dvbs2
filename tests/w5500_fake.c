/* Register-level model of a WIZnet W5500 for tests/test_w5500.py. It implements what
 * firmware/components/w5500 relies on, as read in the W5500 Datasheet v1.1.0 (DS):
 *   - SPI frames: 16-bit offset, control byte BSB/RWB/OM, data phase (DS 2.2, pp.14-17);
 *   - common and socket register blocks with their reset values (DS 3.1-3.2, 4.1-4.2);
 *   - 16 KB TX and RX memories split by Sn_TXBUF_SIZE/Sn_RXBUF_SIZE in socket order, 16-bit
 *     offsets mapped modulo the buffer size (DS 3.3 p.31, pp.52-53);
 *   - UDP OPEN, CLOSE, SEND, RECV with free-running 16-bit pointers (DS pp.46-48, 53-56), the
 *     8-byte receive header (WIZnet "W5500 UDP Function" page), ARP for unicast SENDs with the
 *     RTR x (RCR + 1) timeout (DS p.38).
 * Time: every frame advances a clock by a model of the ESP backend's cost and then byte by byte at
 * f_SCK, so the chip can act between two bytes of one frame. Two backends are modelled: the
 * ESP-IDF one (polled frames up to small_max bytes, interrupt DMA transactions above) and the
 * direct GP-SPI2 one (posting = 1: payload writes are posted and land in the chip when their bits
 * have clocked out; the next frame waits for them, sleeping on an interrupt when the wait is
 * longer than sleep_min_us; payload reads sleep the same way). A SEND holds the wire for its
 * Ethernet frame. Sn_CR clears cmd_us after a command (acceptance, DS p.46) and Sn_SR changes
 * state_us after OPEN or CLOSE (completion, DS pp.46-49), independently. Datagrams can be
 * published in two steps (Sn_RX_WR first covering part of one) and Sn_RX_WR can change between the
 * two bytes of a read. What the datasheet forbids, and what the driver must never do, is counted
 * as a violation with a reason. After a TIMEOUT the model leaves Sn_TX_RD where it was (the
 * datasheet does not say). */
#include "w5500.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM 16384
#define LOG 2048
#define TRACE 256

typedef struct {
    uint8_t ip[4];
    uint16_t port, sport, len;
    uint8_t data[W5500_UDP_MAX];
} dgram;

typedef struct {
    uint32_t frames, polled, queued, bytes, violations, rx_dropped, arp_requests, resets, logged;
    uint32_t waits;             /* frames that waited for a posted write to finish */
    double now_us, cpu_us;
} fake_stats;

typedef struct {                /* a posted payload write still clocking out */
    const uint8_t *tx;
    double done;
    uint16_t offset, n;
    uint8_t control;
    bool on;
} posted_write;

typedef struct {
    uint16_t offset, n;
    uint8_t control;
} trace_entry;

typedef struct {
    uint8_t common[0x40];
    uint8_t reg[8][0x30];
    uint16_t committed[8];      /* Sn_RX_RD as of the last RECV */
    int sending[8];             /* 0 idle, 1 on the wire, 2 ARP that will time out */
    double send_done[8], cmd_done[8];
    double wire_free;      /* the one wire: a SEND starts when the last one has left */
    int state_to[8];            /* Sn_SR after a pending OPEN or CLOSE, -1 if none */
    double state_at[8];
    double publish_at;          /* a datagram's second part appears in Sn_RX_WR */
    int publish_sock;
    uint16_t publish_wr;
    unsigned churn;             /* Sn_RX_WR reads that see a tiny datagram arrive mid-read */
    int last_sock;
    uint8_t arp_ip[8][4];
    bool arp_valid[8];
    uint8_t tx[MEM], rx[MEM];
    double sck_hz, poll_us, queue_us, wire_bps, cmd_us, state_us, arp_us, reset_us, reset_until;
    double wake_us, wake_lat_us, sleep_min_us;
    unsigned small_max;
    bool posting;
    posted_write posted;
    bool arp_reply;
    uint8_t version, phy, mosi_and;
    uint16_t open_ptr;
    int fail_in;
    unsigned sit;          /* the next SENDs are accepted (Sn_CR cleared) but not run */
    bool tear;
    dgram tear_dg;
    dgram *log;
    unsigned log_head, log_count;
    trace_entry trace[TRACE];
    unsigned trace_count;
    fake_stats st;
    char why[200];
    w5500 w;
} fake;

static void violate(fake *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->why, sizeof f->why, fmt, ap);
    va_end(ap);
    f->st.violations++;
}

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static unsigned bytes(fake *f, unsigned s, bool tx) { return f->reg[s][tx ? 0x1F : 0x1E] * 1024u; }

static unsigned base(fake *f, unsigned s, bool tx)
{
    unsigned b = 0;
    for (unsigned k = 0; k < s; ++k) b += bytes(f, k, tx);
    return b;
}

static bool sizes_valid(fake *f)
{
    for (int tx = 0; tx < 2; ++tx) {
        unsigned sum = 0;
        for (unsigned s = 0; s < 8; ++s) {
            const unsigned kb = f->reg[s][tx ? 0x1F : 0x1E];
            if (kb > 16 || (kb & (kb - 1))) return false;
            sum += kb;
        }
        if (sum > 16) return false;
    }
    return true;
}

static void reset(fake *f)
{
    memset(f->common, 0, sizeof f->common);
    w16(f->common + 0x19, 0x07D0);   /* RTR, DS p.38 */
    f->common[0x1B] = 0x08;          /* RCR */
    f->common[0x1C] = 0x28;          /* PTIMER */
    w16(f->common + 0x26, 0xFFFF);   /* PMRU */
    f->common[0x2E] = 0xB8;          /* PHYCFGR 0b10111xxx */
    for (unsigned s = 0; s < 8; ++s) {
        uint8_t *r = f->reg[s];
        memset(r, 0, 0x30);
        memset(r + 0x06, 0xFF, 6);   /* Sn_DHAR */
        r[0x16] = 0x80;              /* Sn_TTL */
        r[0x1E] = r[0x1F] = 2;       /* 2 KB each */
        r[0x2C] = 0xFF;              /* Sn_IMR */
        w16(r + 0x2D, 0x4000);       /* Sn_FRAG */
        f->sending[s] = 0;
        f->committed[s] = 0;
        f->cmd_done[s] = 0;
        f->state_to[s] = -1;
        f->arp_valid[s] = false;
    }
    f->publish_sock = -1;
}

fake *fake_new(void)
{
    fake *f = calloc(1, sizeof *f);
    f->log = calloc(LOG, sizeof *f->log);
    f->sck_hz = 80e6 / 3;
    f->poll_us = 11;
    f->queue_us = 26;
    f->wire_bps = 100e6;
    f->cmd_us = 1;
    f->arp_us = 500;
    f->reset_us = 50;
    f->arp_reply = true;
    f->version = W5500_VERSION;
    f->phy = W5500_PHY_LINK | W5500_PHY_100M | W5500_PHY_FULL;
    f->mosi_and = 0xFF;
    f->fail_in = -1;
    f->small_max = W5500_SMALL_MAX;
    f->wake_us = 6;
    f->wake_lat_us = 2;
    f->sleep_min_us = 40;
    reset(f);
    return f;
}

void fake_free(fake *f)
{
    free(f->log);
    free(f);
}

int fake_set(fake *f, const char *key, double v)
{
    if (!strcmp(key, "sck_hz")) f->sck_hz = v;
    else if (!strcmp(key, "poll_us")) f->poll_us = v;
    else if (!strcmp(key, "queue_us")) f->queue_us = v;
    else if (!strcmp(key, "wire_bps")) f->wire_bps = v;
    else if (!strcmp(key, "cmd_us")) f->cmd_us = v;
    else if (!strcmp(key, "state_us")) f->state_us = v;
    else if (!strcmp(key, "churn")) f->churn = (unsigned)v;
    else if (!strcmp(key, "posting")) f->posting = v != 0;
    else if (!strcmp(key, "small_max")) f->small_max = (unsigned)v;
    else if (!strcmp(key, "wake_us")) f->wake_us = v;
    else if (!strcmp(key, "wake_lat_us")) f->wake_lat_us = v;
    else if (!strcmp(key, "sleep_min_us")) f->sleep_min_us = v;
    else if (!strcmp(key, "arp_us")) f->arp_us = v;
    else if (!strcmp(key, "arp_reply")) f->arp_reply = v != 0;
    else if (!strcmp(key, "reset_us")) f->reset_us = v;
    else if (!strcmp(key, "version")) f->version = (uint8_t)v;
    else if (!strcmp(key, "phy")) f->phy = (uint8_t)v;
    else if (!strcmp(key, "mosi_and")) f->mosi_and = (uint8_t)v;
    else if (!strcmp(key, "open_ptr")) f->open_ptr = (uint16_t)v;
    else if (!strcmp(key, "fail_in")) f->fail_in = (int)v;
    else if (!strcmp(key, "sit")) f->sit = (unsigned)v;
    else return -1;
    return 0;
}

const char *fake_why(fake *f) { return f->why; }
void fake_get_stats(fake *f, fake_stats *out) { *out = f->st; out->logged = f->log_count; }

static void complete(fake *f, unsigned s)
{
    uint8_t *r = f->reg[s];
    if (f->sending[s] == 2) {
        r[0x02] |= 0x08;   /* TIMEOUT; Sn_TX_RD stays */
        f->sending[s] = 0;
        return;
    }
    const uint16_t rd = r16(r + 0x22), wr = r16(r + 0x24), n = (uint16_t)(wr - rd);
    const unsigned size = bytes(f, s, true), b = base(f, s, true);
    if (f->log_count == LOG) {
        violate(f, "datagram log full");
    } else {
        dgram *d = &f->log[(f->log_head + f->log_count++) % LOG];
        memcpy(d->ip, r + 0x0C, 4);
        d->port = r16(r + 0x10);
        d->sport = r16(r + 0x04);
        d->len = n;
        for (unsigned i = 0; i < n && i < W5500_UDP_MAX; ++i)
            d->data[i] = f->tx[b + ((uint16_t)(rd + i) & (size - 1))];
    }
    w16(r + 0x22, wr);
    r[0x02] |= 0x10;       /* SEND_OK */
    f->sending[s] = 0;
}

static void settle_state(fake *f, unsigned s)
{
    uint8_t *r = f->reg[s];
    if (f->state_to[s] == 0x22) {
        r[0x03] = 0x22;
        w16(r + 0x22, f->open_ptr);
        w16(r + 0x24, f->open_ptr);
        w16(r + 0x28, f->open_ptr);
        w16(r + 0x2A, f->open_ptr);
        f->committed[s] = f->open_ptr;
        f->arp_valid[s] = false;
    } else {
        r[0x03] = 0;
    }
    f->sending[s] = 0;
    f->state_to[s] = -1;
}

static void put(fake *f, unsigned bsb, uint16_t a, uint8_t v);

/* A posted write reaches the chip when its last bit has clocked out; it is read from the
 * caller's buffer only then, as a DMA would, so a buffer changed too early shows up. */
static void land(fake *f)
{
    posted_write *p = &f->posted;
    p->on = false;
    for (unsigned i = 0; i < p->n; ++i)
        put(f, p->control >> 3, (uint16_t)(p->offset + i), p->tx[i] & f->mosi_and);
}

static void update(fake *f)
{
    if (f->posted.on && f->st.now_us >= f->posted.done) land(f);
    for (;;) {             /* finished SENDs in the order they left the wire */
        int first = -1;
        for (unsigned s = 0; s < 8; ++s)
            if (f->sending[s] && f->st.now_us >= f->send_done[s] &&
                (first < 0 || f->send_done[s] < f->send_done[first])) first = (int)s;
        if (first < 0) break;
        complete(f, (unsigned)first);
    }
    for (unsigned s = 0; s < 8; ++s)
        if (f->state_to[s] >= 0 && f->st.now_us >= f->state_at[s]) settle_state(f, s);
    if (f->publish_sock >= 0 && f->st.now_us >= f->publish_at) {
        w16(f->reg[f->publish_sock] + 0x2A, f->publish_wr);
        f->publish_sock = -1;
    }
}

void fake_advance(fake *f, double us)
{
    f->st.now_us += us;
    update(f);
}

static void delay(void *ctx, uint32_t us)
{
    fake *f = ctx;
    f->st.cpu_us += us;   /* the backend busy-waits */
    fake_advance(f, us);
}

static void command(fake *f, unsigned s, uint8_t v)
{
    uint8_t *r = f->reg[s];
    if (f->st.now_us < f->cmd_done[s])
        violate(f, "S%u_CR 0x%02x before 0x%02x was accepted", s, v, r[1]);
    if (f->state_to[s] >= 0) violate(f, "S%u command 0x%02x before OPEN or CLOSE completed", s, v);
    r[0x01] = v;
    f->cmd_done[s] = f->st.now_us + f->cmd_us;
    switch (v) {
    case 0x01:   /* OPEN */
        if ((r[0x00] & 0x0F) != 0x02) {
            violate(f, "S%u OPEN without UDP mode", s);
            return;
        }
        if (!sizes_valid(f)) violate(f, "socket buffer sizes invalid or over 16 KB");
        if (r[0x03]) violate(f, "S%u OPEN while not SOCK_CLOSED", s);
        f->state_to[s] = 0x22;
        f->state_at[s] = f->st.now_us + f->state_us;
        update(f);
        return;
    case 0x10:   /* CLOSE */
        f->state_to[s] = 0;
        f->state_at[s] = f->st.now_us + f->state_us;
        update(f);
        return;
    case 0x20: { /* SEND */
        if (r[0x03] != 0x22) {
            violate(f, "S%u SEND while not open in UDP mode", s);
            return;
        }
        if (f->sending[s]) {
            violate(f, "S%u SEND while a SEND is in progress", s);
            return;
        }
        if (f->sit) {       /* as the board's chip did at the end of some egress runs */
            f->sit--;
            return;
        }
        const unsigned n = (uint16_t)(r16(r + 0x24) - r16(r + 0x22));
        if (!n || n > bytes(f, s, true)) violate(f, "S%u SEND of %u bytes", s, n);
        if (n > W5500_UDP_MAX) violate(f, "S%u SEND of %u bytes splits into datagrams", s, n);
        const unsigned frame = (n + 46 < 64 ? 64 : n + 46) + 20;   /* + preamble, SFD, gap */
        const double wire = frame * 8.0 / f->wire_bps * 1e6;
        static const uint8_t all[4] = {255, 255, 255, 255};
        f->sending[s] = 1;
        if (!memcmp(r + 0x0C, all, 4) ||
            (f->arp_valid[s] && !memcmp(f->arp_ip[s], r + 0x0C, 4))) {
            const double start = f->st.now_us > f->wire_free ? f->st.now_us : f->wire_free;
            f->send_done[s] = f->wire_free = start + wire;
        } else if (f->arp_reply) {
            f->st.arp_requests++;
            memcpy(f->arp_ip[s], r + 0x0C, 4);
            f->arp_valid[s] = true;
            const double ready = f->st.now_us + f->arp_us;
            f->send_done[s] = f->wire_free = (ready > f->wire_free ? ready : f->wire_free) + wire;
        } else {
            f->st.arp_requests++;
            f->sending[s] = 2;
            f->send_done[s] = f->st.now_us + r16(f->common + 0x19) * 100.0 * (f->common[0x1B] + 1);
        }
        return;
    }
    case 0x40: { /* RECV */
        if (r[0x03] != 0x22) {
            violate(f, "S%u RECV while not open in UDP mode", s);
            return;
        }
        const uint16_t rd = r16(r + 0x28), old = f->committed[s], wr = r16(r + 0x2A);
        if ((uint16_t)(rd - old) > (uint16_t)(wr - old)) violate(f, "S%u_RX_RD beyond Sn_RX_WR", s);
        f->committed[s] = rd;
        return;
    }
    default:
        violate(f, "S%u unexpected command 0x%02x", s, v);
    }
}

static int inject(fake *f, const uint8_t ip[4], uint16_t sport, uint16_t dport, const uint8_t *data,
                  size_t n)
{
    if (n > W5500_UDP_MAX) return -2;
    for (unsigned s = 0; s < 8; ++s) {
        uint8_t *r = f->reg[s];
        if (r[0x03] != 0x22 || r16(r + 0x04) != dport || !bytes(f, s, false)) continue;
        const unsigned size = bytes(f, s, false), b = base(f, s, false);
        const uint16_t wr = r16(r + 0x2A);
        if ((uint16_t)(wr - f->committed[s]) + 8 + n > size) {
            f->st.rx_dropped++;
            return 0;
        }
        uint8_t head[8] = {ip[0], ip[1], ip[2], ip[3]};
        w16(head + 4, sport);
        w16(head + 6, (uint16_t)n);
        for (unsigned i = 0; i < 8 + n; ++i)
            f->rx[b + ((uint16_t)(wr + i) & (size - 1))] = i < 8 ? head[i] : data[i - 8];
        w16(r + 0x2A, (uint16_t)(wr + 8 + n));
        r[0x02] |= 0x04;   /* RECV */
        f->last_sock = (int)s;
        return 1;
    }
    return -1;
}

int fake_inject(fake *f, const uint8_t ip[4], uint16_t sport, uint16_t dport, const uint8_t *data,
                size_t n)
{
    update(f);
    return inject(f, ip, sport, dport, data, n);
}

/* Store a datagram but let Sn_RX_WR cover only its first `first` bytes until delay_us later. */
int fake_inject_split(fake *f, const uint8_t ip[4], uint16_t sport, uint16_t dport,
                      const uint8_t *data, size_t n, unsigned first, double delay_us)
{
    update(f);
    if (f->publish_sock >= 0) return -3;
    const int r = inject(f, ip, sport, dport, data, n);
    if (r != 1) return r;
    uint8_t *reg = f->reg[f->last_sock];
    f->publish_wr = r16(reg + 0x2A);
    w16(reg + 0x2A, (uint16_t)(f->publish_wr - (8 + n) + first));
    f->publish_sock = f->last_sock;
    f->publish_at = f->st.now_us + delay_us;
    return 1;
}

/* Deliver this datagram between the two bytes of the next Sn_RX_WR read. */
void fake_tear(fake *f, const uint8_t ip[4], uint16_t sport, uint16_t dport, const uint8_t *data,
               size_t n)
{
    f->tear = true;
    memcpy(f->tear_dg.ip, ip, 4);
    f->tear_dg.sport = sport;
    f->tear_dg.port = dport;
    f->tear_dg.len = (uint16_t)n;
    memcpy(f->tear_dg.data, data, n);
}

int fake_take(fake *f, uint8_t ip[4], uint16_t *port, uint16_t *sport, uint8_t *data)
{
    if (!f->log_count) return -1;
    const dgram *d = &f->log[f->log_head];
    f->log_head = (f->log_head + 1) % LOG;
    f->log_count--;
    memcpy(ip, d->ip, 4);
    *port = d->port;
    *sport = d->sport;
    memcpy(data, d->data, d->len);
    return d->len;
}

static uint8_t get(fake *f, unsigned bsb, uint16_t a)
{
    if (!bsb) {
        if (a > 0x39) {
            violate(f, "read of reserved common offset 0x%04x", a);
            return 0;
        }
        if (a == 0x00) return f->st.now_us < f->reset_until ? 0x80 : f->common[0];
        if (a == 0x2E) return (uint8_t)((f->common[0x2E] & 0xF8) | (f->phy & 7));
        if (a == 0x39) return f->version;
        return f->common[a];
    }
    const unsigned s = bsb / 4, kind = bsb % 4;
    uint8_t *r = f->reg[s];
    if (kind == 1) {
        if (a >= 0x30) {
            violate(f, "read of reserved S%u offset 0x%04x", s, a);
            return 0;
        }
        if (a == 0x01) return f->st.now_us < f->cmd_done[s] ? r[1] : 0;
        if (a >= 0x20 && a <= 0x2B && f->state_to[s] >= 0)
            violate(f, "S%u pointer read before OPEN or CLOSE completed", s);
        if (a == 0x20 || a == 0x21) {
            uint8_t v[2];
            w16(v, (uint16_t)(bytes(f, s, true) - (uint16_t)(r16(r + 0x24) - r16(r + 0x22))));
            return v[a - 0x20];
        }
        if (a == 0x26 || a == 0x27) {
            uint8_t v[2];
            w16(v, (uint16_t)(r16(r + 0x2A) - f->committed[s]));
            return v[a - 0x26];
        }
        return r[a];
    }
    const bool tx = kind == 2;
    const unsigned size = bytes(f, s, tx);
    if (!size) {
        violate(f, "S%u %s buffer access with no memory", s, tx ? "TX" : "RX");
        return 0;
    }
    const unsigned at = base(f, s, tx) + (a & (size - 1));
    if (tx) return f->tx[at];
    const uint16_t old = f->committed[s];
    if ((uint16_t)(a - old) >= (uint16_t)(r16(r + 0x2A) - old))
        violate(f, "S%u RX read at 0x%04x past Sn_RX_WR 0x%04x", s, a, r16(r + 0x2A));
    return f->rx[at];
}

static void put(fake *f, unsigned bsb, uint16_t a, uint8_t v)
{
    if (!bsb) {
        if (a == 0x00) {
            if (v & 0x80) {
                reset(f);
                f->reset_until = f->st.now_us + f->reset_us;
                f->st.resets++;
            } else {
                f->common[0] = v;
            }
        } else if (a == 0x15) {
            f->common[0x15] &= (uint8_t)~v;
        } else if (a == 0x2E) {
            f->common[0x2E] = (uint8_t)((v & 0xF8) | (f->common[0x2E] & 7));
        } else if ((a >= 0x28 && a <= 0x2D) || a >= 0x2F) {
            violate(f, "write to read-only or reserved common offset 0x%04x", a);
        } else {
            f->common[a] = v;
        }
        return;
    }
    const unsigned s = bsb / 4, kind = bsb % 4;
    uint8_t *r = f->reg[s];
    if (kind == 1) {
        if (a >= 0x30 || a == 0x03 || (a >= 0x20 && a <= 0x23) || a == 0x26 || a == 0x27 ||
            a == 0x2A || a == 0x2B) {
            violate(f, "write to read-only or reserved S%u offset 0x%04x", s, a);
        } else if (a == 0x01) {
            command(f, s, v);
        } else if (a == 0x02) {
            r[0x02] &= (uint8_t)~v;
        } else {
            if (a >= 0x0C && a <= 0x11 && f->sending[s])
                violate(f, "S%u destination changed during SEND", s);
            if ((a == 0x24 || a == 0x25) && f->sending[s])
                violate(f, "S%u_TX_WR written during SEND", s);
            if ((a == 0x1E || a == 0x1F) && r[0x03])
                violate(f, "S%u buffer size changed while open", s);
            if ((a <= 0x05 || a == 0x1E || a == 0x1F) && f->state_to[s] >= 0)
                violate(f, "S%u reconfigured before OPEN or CLOSE completed", s);
            r[a] = v;
        }
        return;
    }
    if (kind == 3) {
        violate(f, "S%u write to the RX buffer", s);
        return;
    }
    const unsigned size = bytes(f, s, true);
    if (!size) {
        violate(f, "S%u TX write with no memory", s);
        return;
    }
    const uint16_t rd = r16(r + 0x22), used = (uint16_t)(r16(r + 0x24) - rd);
    if (((uint16_t)(a - rd) & (size - 1)) < used)
        violate(f, "S%u TX write at 0x%04x over unsent data", s, a);
    f->tx[base(f, s, true) + (a & (size - 1))] = v;
}

/* The backend's wait for a posted write: spin for a short remainder, else sleep on an interrupt. */
static void wait_posted(fake *f)
{
    if (!f->posted.on) return;
    const double left = f->posted.done - f->st.now_us;
    if (left > 0) {
        f->st.waits++;
        f->st.cpu_us += left > f->sleep_min_us ? f->wake_us : left;
        f->st.now_us = f->posted.done + (left > f->sleep_min_us ? f->wake_lat_us : 0);
    }
    update(f);
}

static bool busy(void *ctx)
{
    fake *f = ctx;
    update(f);
    return f->posted.on;
}

static int frame(void *ctx, uint16_t offset, uint8_t control, const uint8_t *tx, uint8_t *rx,
                 size_t n, bool post)
{
    fake *f = ctx;
    if (f->fail_in == 0) {
        f->fail_in = -1;
        return -1;
    }
    if (f->fail_in > 0) f->fail_in--;
    wait_posted(f);
    const bool polled = n <= f->small_max;
    const double byte_us = 8.0 / f->sck_hz * 1e6, bits_us = (3 + n) * byte_us;
    f->st.frames++;
    f->st.polled += polled;
    f->st.queued += !polled;
    f->st.bytes += (uint32_t)(3 + n);
    f->trace[f->trace_count++ % TRACE] = (trace_entry){offset, (uint16_t)n, control};

    const unsigned bsb = control >> 3, write = control >> 2 & 1, om = control & 3;
    if (om) violate(f, "OM %u: only variable length data mode is used", om);
    if (!n || (tx == NULL) == (rx == NULL)) {
        violate(f, "frame needs n >= 1 and exactly one of tx and rx");
        return 0;
    }
    if (write != (tx != NULL)) violate(f, "RWB %u disagrees with the transfer direction", write);
    if (bsb && bsb % 4 == 0) {
        violate(f, "reserved block select %u", bsb);
        return 0;
    }
    if (post && !write) violate(f, "posted read");
    if (!polled && post && f->posting) {
        f->st.now_us += f->queue_us;
        f->st.cpu_us += f->queue_us;
        f->posted = (posted_write){tx, f->st.now_us + bits_us, offset, (uint16_t)n, control, true};
        return 0;
    }
    bool sleep = false;
    if (polled) {
        f->st.cpu_us += f->poll_us + bits_us;
        f->st.now_us += f->poll_us;
    } else if (f->posting) {    /* direct backend: a payload read sleeps until it is done */
        sleep = bits_us > f->sleep_min_us;
        f->st.cpu_us += f->queue_us + (sleep ? f->wake_us : bits_us);
        f->st.now_us += f->queue_us;
    } else {                    /* ESP-IDF interrupt transaction */
        f->st.cpu_us += f->queue_us;
        f->st.now_us += f->queue_us;
    }
    f->st.now_us += 3 * byte_us;   /* to the data phase */
    update(f);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t a = (uint16_t)(offset + i);
        f->st.now_us += byte_us;
        update(f);
        if (write) {
            put(f, bsb, a, tx[i] & f->mosi_and);
            continue;
        }
        rx[i] = get(f, bsb, a);
        if (f->tear && bsb % 4 == 1 && a == 0x2A &&
            r16(f->reg[bsb / 4] + 0x04) == f->tear_dg.port) {
            f->tear = false;
            inject(f, f->tear_dg.ip, f->tear_dg.sport, f->tear_dg.port, f->tear_dg.data,
                   f->tear_dg.len);
        }
        if (f->churn && bsb % 4 == 1 && a == 0x2A) {
            static const uint8_t from[4] = {10, 0, 0, 9};
            f->churn--;
            inject(f, from, 7777, r16(f->reg[bsb / 4] + 0x04), (const uint8_t *)"tiny", 4);
        }
    }
    if (sleep) f->st.now_us += f->wake_lat_us;
    return 0;
}

/* One frame straight from the test, to show that the checks above fire. */
int fake_frame_raw(fake *f, uint16_t offset, uint8_t control, const uint8_t *tx, uint8_t *rx,
                   size_t n)
{
    return frame(f, offset, control, tx, rx, n, false);
}

/* Raw register and memory access for the test, without side effects or checks. */
int fake_peek(fake *f, unsigned bsb, unsigned a)
{
    if (!bsb) return a == 0x39 ? f->version : f->common[a & 0x3F];
    const unsigned s = bsb / 4, kind = bsb % 4;
    if (kind == 1) return f->reg[s][a % 0x30];
    const unsigned size = bytes(f, s, kind == 2);
    if (!size) return -1;
    const unsigned at = base(f, s, kind == 2) + (a & (size - 1));
    return kind == 2 ? f->tx[at] : f->rx[at];
}

void fake_poke(fake *f, unsigned bsb, unsigned a, unsigned v)
{
    const unsigned s = bsb / 4, kind = bsb % 4;
    if (!bsb) f->common[a & 0x3F] = (uint8_t)v;
    else if (kind == 1) f->reg[s][a % 0x30] = (uint8_t)v;
    else if (bytes(f, s, kind == 2)) {
        const unsigned size = bytes(f, s, kind == 2);
        (kind == 2 ? f->tx : f->rx)[base(f, s, kind == 2) + (a & (size - 1))] = (uint8_t)v;
    }
}

int fake_physical(fake *f, unsigned s, int tx)
{
    return (int)base(f, s, tx != 0);
}

/* Frame i (counted from the start) while it is among the last TRACE frames. */
int fake_trace(fake *f, unsigned i, uint16_t *offset, uint8_t *control, uint16_t *n)
{
    if (i >= f->trace_count || f->trace_count - i > TRACE) return -1;
    const unsigned k = i % TRACE;
    *offset = f->trace[k].offset;
    *control = f->trace[k].control;
    *n = f->trace[k].n;
    return 0;
}

unsigned fake_trace_count(fake *f) { return f->trace_count; }

/* The driver under test, wired to this fake. */
int fake_driver_init(fake *f, const uint8_t net[18], const uint8_t kb[4])
{
    const w5500_bus bus = {.frame = frame, .delay_us = delay, .busy = f->posting ? busy : NULL,
                           .ctx = f};
    w5500_config c = {0};
    memcpy(c.gateway, net, 4);
    memcpy(c.mask, net + 4, 4);
    memcpy(c.mac, net + 8, 6);
    memcpy(c.ip, net + 14, 4);
    c.tx_kb[0] = kb[0];
    c.rx_kb[0] = kb[1];
    c.tx_kb[1] = kb[2];
    c.rx_kb[1] = kb[3];
    return w5500_init(&f->w, &bus, &c);
}

w5500 *fake_driver(fake *f) { return &f->w; }

void fake_driver_counters(fake *f, uint32_t out[3])
{
    out[0] = f->w.frames;
    out[1] = f->w.frame_bytes;
    out[2] = f->w.bus_errors;
}

size_t fake_driver_size(void) { return sizeof(w5500); }
