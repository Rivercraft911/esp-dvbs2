/* TS deframer: single transport stream BBFRAMEs back into 188-byte packets.
 *
 * EN 302 307-1 V1.4.1 5.1.4 to 5.1.6, TS/GS = 11, UPL = 188 x 8, SYNC = 0x47. The modulator's
 * mode adaptation put the CRC-8 of each packet's 187 bytes after its sync byte into the sync
 * position of the next packet, then cut the stream into data fields wherever a frame ends.
 * SYNCD is the bit offset of the first packet that starts in a data field (65,535: none). So a
 * packet can be checked only when the first byte of the packet after it arrives, which may be in
 * the next frame, and a packet may lie across two frames. Packets that lie inside one data field
 * go out from it, their sync bytes restored in place; the deframer copies at most one packet
 * (188 bytes) from frame to frame.
 *
 * LeanDVB's deframer (GPL) was a behavioural reference only; this follows the standard.
 * Choices, where the standard leaves them to the receiver:
 *   - A CRC-8 mismatch sets the packet's transport_error_indicator (ISO/IEC 13818-1 2.4.3.2), so a
 *     decoder can conceal it, and the packet still goes on. Frames reach this deframer only after
 *     their BCH check, so a mismatch points at the transmitter or this code, not the channel.
 *   - Frames must come in order with consecutive sequence numbers. A gap restarts the deframer at
 *     the frame's SYNCD; a complete packet still waiting for its CRC-8 goes on unchecked, a
 *     partial one is dropped. A frame no newer than the last (the LDPC decoder's, late) is
 *     ignored: its packets have been given up for.
 *   - A SYNCD that disagrees with the packet carried over also restarts: the carried packet is
 *     dropped, since the two frames do not fit together.
 *   - Continuity counters are checked on the PIDs seen first, up to RX_TS_PIDS, null packets
 *     aside. A packet that failed its CRC-8 is checked too: its header is more likely intact than
 *     not (4 of its 188 bytes), and skipping it would turn the next packet into a jump. */
#include "dvbs2rx.h"

#include <string.h>

/* The CRC-8 loop keeps its own section, so linker.lf can place it in IRAM. */
#ifdef ESP_PLATFORM
#define HOT __attribute__((noinline, noclone))
#else
#define HOT
#endif

#define NO_SYNCD 0xffffu
#define NULL_PID 0x1fffu

typedef struct {
    uint16_t pid;
    uint8_t cc, duplicate, payload;   /* payload: the last packet carried one */
    uint32_t print;                   /* fingerprint of the last packet with payload */
    uint32_t packets, cc_errors;
} tracked_t;

struct rx_ts {
    uint8_t carry[RX_TS_PACKET];  /* the packet across frames, from its sync position */
    unsigned have;                /* bytes of it; RX_TS_PACKET: whole, its CRC-8 comes next */
    uint8_t crc;                  /* CRC-8 of carry[1 .. have) */
    uint8_t synced, started, drop_null;
    uint32_t sequence;            /* the last frame's */
    unsigned pids;
    tracked_t pid[RX_TS_PIDS];
    rx_ts_emit_fn emit;
    void *ctx;
    rx_ts_stats_t st;
    uint8_t crc8[256];            /* 5.1.4: g(X) = X^8 + X^7 + X^6 + X^4 + X^2 + 1, MSB first */
};

size_t rx_ts_size(void)
{
    return sizeof(rx_ts_t);
}

void rx_ts_init(rx_ts_t *t, int drop_null, rx_ts_emit_fn emit, void *ctx)
{
    memset(t, 0, sizeof *t);
    t->drop_null = (uint8_t)(drop_null != 0);
    t->emit = emit;
    t->ctx = ctx;
    for (unsigned v = 0; v < 256; ++v) {
        unsigned c = v;
        for (unsigned b = 0; b < 8; ++b) {
            c = ((c << 1) ^ (c & 0x80 ? 0xd5 : 0)) & 0xff;
        }
        t->crc8[v] = (uint8_t)c;
    }
}

/* Four bytes a step: the table lookups chain, the byte loads and the loop count do not. */
static HOT uint8_t crc8(const uint8_t *table, unsigned c, const uint8_t *p, unsigned n)
{
    const uint8_t *end = p + n;
    for (; end - p >= 4; p += 4) {
        c = table[c ^ p[0]];
        c = table[c ^ p[1]];
        c = table[c ^ p[2]];
        c = table[c ^ p[3]];
    }
    while (p != end) {
        c = table[c ^ *p++];
    }
    return (uint8_t)c;
}

/* FNV-1a over bytes 1..187 two at a time, the PCR left out: a duplicate packet repeats every
 * byte of the original except that its PCR may be new (ISO/IEC 13818-1 2.4.3.3). The PCR is
 * bytes 6..11 when the adaptation field (byte 4, its length; byte 5, its flags) has one. */
static uint32_t fingerprint(const uint8_t *p)
{
    const int pcr = (p[3] & 0x20) && p[4] >= 7 && (p[5] & 0x10);
    uint32_t h = (2166136261u ^ p[1]) * 16777619u;
    for (unsigned k = 2; k < RX_TS_PACKET; k += 2) {
        if (pcr && k == 6) {
            k = 12;
        }
        h = (h ^ (p[k] | (uint32_t)p[k + 1] << 8)) * 16777619u;
    }
    return h;
}

/* ISO/IEC 13818-1 2.4.3.3: the counter steps by one per packet with payload and stays put on a
 * packet without one. A packet with payload may be sent twice, never more, with the same counter
 * and the same bytes (its PCR aside). The counter may jump where the adaptation field's
 * discontinuity_indicator is set. */
static void continuity(rx_ts_t *t, const uint8_t *p, unsigned pid)
{
    tracked_t *e = t->pid, *end = t->pid + t->pids;
    while (e != end && e->pid != pid) {
        ++e;
    }
    if (e == end) {
        if (t->pids == RX_TS_PIDS) {
            ++t->st.untracked;
            return;
        }
        ++t->pids;
        e->pid = (uint16_t)pid;
        e->cc = p[3] & 15;
        e->payload = p[3] >> 4 & 1;
        e->print = e->payload ? fingerprint(p) : 0;
        e->packets = 1;
        return;
    }
    ++e->packets;
    const unsigned cc = p[3] & 15, payload = p[3] >> 4 & 1;
    const int discontinuity = (p[3] & 0x20) && p[4] && (p[5] & 0x80);
    const uint32_t print = payload ? fingerprint(p) : 0;
    int ok;
    if (!payload) {
        ok = cc == e->cc;
    } else if (cc == ((e->cc + 1u) & 15)) {
        ok = 1;
    } else if (cc == e->cc && e->payload && !e->duplicate && print == e->print) {
        e->duplicate = 1;                       /* the one duplicate allowed */
        return;
    } else {
        ok = 0;
    }
    if (!ok && !discontinuity) {
        ++e->cc_errors;
        ++t->st.cc_errors;
    }
    e->cc = (uint8_t)cc;
    e->duplicate = 0;
    e->payload = (uint8_t)payload;
    if (payload) {
        e->print = print;
    }
}

/* A whole packet: restore its sync byte, mark it if its CRC-8 failed, check it and hand it on.
 * checked: 1 matched, 0 failed, -1 nothing to check against. */
static void deliver(rx_ts_t *t, uint8_t *p, int checked)
{
    p[0] = 0x47;
    if (!checked) {
        p[1] |= 0x80;
        ++t->st.crc_errors;
    } else if (checked < 0) {
        ++t->st.unchecked;
    }
    const unsigned pid = (p[1] & 0x1fu) << 8 | p[2];
    if (pid == NULL_PID) {
        ++t->st.null_packets;
        if (t->drop_null) {
            return;
        }
    } else {
        continuity(t, p, pid);
    }
    ++t->st.packets;
    t->emit(t->ctx, p);
}

/* Give up the carried packet (on, unchecked, if whole and kept) and wait for a packet start. */
static void stop(rx_ts_t *t, int keep)
{
    if (t->synced && t->have == RX_TS_PACKET && keep) {
        deliver(t, t->carry, -1);
    } else {
        t->st.dropped_bytes += t->have;
    }
    t->have = 0;
    t->synced = 0;
}

static void restart(rx_ts_t *t, int keep)
{
    t->st.resyncs += t->synced;
    stop(t, keep);
}

unsigned rx_ts_frame(rx_ts_t *t, uint8_t *bbframe, unsigned bytes, uint32_t sequence)
{
    const uint32_t before = t->st.packets;
    if (t->started && (int32_t)(sequence - t->sequence) <= 0) {
        ++t->st.late;
        return 0;
    }
    if (t->started && sequence != t->sequence + 1) {
        restart(t, 1);
    }
    t->started = 1;
    t->sequence = sequence;
    const uint8_t *h = bbframe;
    const unsigned upl = (unsigned)h[2] << 8 | h[3], dfl = (unsigned)h[4] << 8 | h[5];
    const unsigned syncd = (unsigned)h[7] << 8 | h[8];
    if ((h[0] & 0xec) != 0xe0 || upl != 8 * RX_TS_PACKET || h[6] != 0x47 || dfl != 8 * bytes ||
        (syncd != NO_SYNCD && (syncd % 8 || syncd / 8 >= bytes))) {
        ++t->st.not_ts;
        restart(t, 1);
        t->st.dropped_bytes += bytes;
        return 0;
    }
    ++t->st.frames;
    uint8_t *field = bbframe + 10;
    const unsigned start = syncd == NO_SYNCD ? bytes : syncd / 8;   /* first packet start */
    unsigned p = 0;
    if (t->synced) {
        /* The carried packet ends after `need` more bytes, where the next one starts. */
        const unsigned need = RX_TS_PACKET - t->have, n = need < bytes ? need : bytes;
        if (start != n) {
            ++t->st.syncd_errors;
            restart(t, 0);
        } else {
            memcpy(t->carry + t->have, field, n);
            t->crc = crc8(t->crc8, t->crc, field, n);
            t->have += n;
            p = n;
            if (p == bytes) {
                return t->st.packets - before;  /* partial still, or whole with its CRC-8 next */
            }
            deliver(t, t->carry, t->crc == field[p]);
            t->have = 0;
        }
    }
    if (!t->synced) {
        if (start == bytes) {
            t->st.dropped_bytes += bytes;   /* no packet starts here */
            return 0;
        }
        t->st.dropped_bytes += start;
        p = start;              /* its first byte checks a packet this deframer never had */
        t->synced = 1;
    }
    for (; p + RX_TS_PACKET < bytes; p += RX_TS_PACKET) {
        uint8_t *q = field + p;
        deliver(t, q, crc8(t->crc8, 0, q + 1, RX_TS_PACKET - 1) == q[RX_TS_PACKET]);
    }
    t->have = bytes - p;
    memcpy(t->carry, field + p, t->have);
    t->crc = crc8(t->crc8, 0, t->carry + 1, t->have - 1);
    return t->st.packets - before;
}

void rx_ts_flush(rx_ts_t *t)
{
    stop(t, 1);
}

void rx_ts_stats(const rx_ts_t *t, rx_ts_stats_t *stats)
{
    *stats = t->st;
}

int rx_ts_pid(const rx_ts_t *t, unsigned i, rx_ts_pid_t *pid)
{
    if (i >= t->pids) {
        return 0;
    }
    pid->pid = t->pid[i].pid;
    pid->packets = t->pid[i].packets;
    pid->cc_errors = t->pid[i].cc_errors;
    return 1;
}
