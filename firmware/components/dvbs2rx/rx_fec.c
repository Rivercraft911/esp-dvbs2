/* Short-frame FEC for the receiver: check the hard bits, decode only if needed.
 *
 * Clean path. The received information bits are re-encoded with the transmitter's
 * fast encoder (components/dvbs2, PIE on the S3) and both parity fields compared;
 * then the BBHEADER is descrambled and its CRC-8 and fields checked, and the data
 * field descrambled in place. Nothing reads the LLRs.
 *
 * Decoder. Layered min-sum with int8 beliefs and messages (EN 302 307-1 Annex C
 * codes; rx_fec_tables.h has the derivation). A row group of q is a layer of 360
 * independent checks, held as 23 vectors of 16 lanes (lanes 360..367 are padding:
 * they compute garbage that is never read back). Every edge of a layer is a column
 * group read at a cyclic shift. Each group's 360 beliefs live in one 384-byte slot
 * in the frame of the last edge that wrote it (position p holds bit p - frame), with
 * positions 0..15 repeated at 360..375 so any 16-byte window is contiguous. Pass 1
 * reads every window of the layer, pass 2 writes each group back aligned in its
 * edge's frame, so loads may be unaligned but stores never are. All frames repeat
 * every iteration and are fixed at init.
 *
 * A group can appear twice or three times in one row group (3/5 and 5/6). Its
 * first edge writes the slot; each repeat stores its message change into a scratch
 * slot, which is then added to the slot read at the relative shift.
 * Check 0 has no p_(-1): layer 0's edge to parity group q - 1 has one lane, check 0,
 * that would read p_(M-1). That lane sees +127 with message 0, so it never sets a
 * minimum or a sign, and p_(M-1) is restored after the layer. */
#include "dvbs2rx.h"
#include "dvbs2.h"
#include "rx_fec_internal.h"
#include "rx_fec_tables.h"

#include <string.h>

#define FIRST_MODCOD 4
#define LAST_MODCOD 10
#define MAX_ITERATIONS 30

static unsigned rate_index(unsigned modcod) { return modcod - FIRST_MODCOD; }

static const dvbs2_mode *short_mode(unsigned modcod)
{
    return modcod >= FIRST_MODCOD && modcod <= LAST_MODCOD ? dvbs2_find_mode_short(modcod) : NULL;
}

static size_t round16(size_t n) { return (n + 15) & ~(size_t)15; }

/* Layout of the workspace after the header, in bytes, for one MODCOD. */
typedef struct {
    size_t belief, r, qtmp, reads, edges, prbs, encoder, bits, total;
} layout_t;

/* Edges in all row groups; the largest row group and most repeated groups in one. */
static unsigned count_edges(unsigned k, unsigned q, unsigned *most, unsigned *repeats)
{
    const uint16_t *e = rx_edges[k];
    unsigned total = 0;
    *most = *repeats = 0;
    for (unsigned r = 0; r < q; ++r, e += 1 + e[0]) {
        unsigned seen = 0;
        for (unsigned j = 0; j < e[0]; ++j) {
            for (unsigned i = 0; i < j; ++i) {
                if (e[1 + i] >> 9 == e[1 + j] >> 9) {
                    ++seen;
                    break;
                }
            }
        }
        total += e[0];
        *most = e[0] > *most ? e[0] : *most;
        *repeats = seen > *repeats ? seen : *repeats;
    }
    return total;
}

static layout_t layout(const dvbs2_mode *mode)
{
    unsigned most, repeats;
    const unsigned k = rate_index(mode->modcod), edges = count_edges(k, mode->q, &most, &repeats);
    layout_t l;
    l.belief = round16(sizeof(rx_fec_t));
    l.r = l.belief + (size_t)RX_SLOT * (RX_GROUPS + repeats);
    l.qtmp = l.r + (size_t)edges * RX_VECTORS * 16;
    l.reads = l.qtmp + (size_t)most * RX_VECTORS * 16;
    l.edges = l.reads + round16((size_t)edges * RX_VECTORS * 2);
    l.prbs = l.edges + round16(edges * sizeof(rx_edge_t));
    l.encoder = l.prbs + round16(mode->kbch / 8);
    l.bits = l.encoder + round16(dvbs2_fast_mode_size(mode));
    l.total = l.bits + DVBS2_SHORT_FAST_WORKSPACE_USED;
    return l;
}

size_t rx_fec_size(unsigned modcod)
{
    const dvbs2_mode *mode = short_mode(modcod);
    return mode ? layout(mode).total : 0;
}

/* ---------------------------------------------------------------- init */

int rx_fec_init(rx_fec_t *f, unsigned modcod)
{
    const dvbs2_mode *mode = short_mode(modcod);
    if (!f || ((uintptr_t)f & 15) || !mode) {
        return -1;
    }
    const unsigned k = rate_index(modcod);
    const layout_t l = layout(mode);
    memset(f, 0, l.total);
    uint8_t *base = (uint8_t *)f;
    f->modcod = modcod;
    f->kbch = mode->kbch;
    f->nbch = mode->nbch;
    f->q = mode->q;
    f->groups = mode->nbch / 360;
    unsigned repeats;
    f->edge_count = count_edges(k, f->q, &f->most, &repeats);
    f->belief = (int8_t *)(base + l.belief);
    f->r = (int8_t *)(base + l.r);
    f->qtmp = (int8_t *)(base + l.qtmp);
    f->reads = (uint16_t *)(base + l.reads);
    f->edges = (rx_edge_t *)(base + l.edges);
    f->prbs = base + l.prbs;
    f->encoder = (dvbs2_fast *)(base + l.encoder);
    f->bits = (dvbs2_workspace *)(base + l.bits);
    if (dvbs2_fast_prepare(f->encoder, mode, 1, DVBS2_PIE_BCH | DVBS2_PIE_LDPC)) {
        return -1;
    }
    f->max_iterations = MAX_ITERATIONS;
    f->scale = RX_SCALE;
    f->offset = RX_OFFSET;
    f->rmax = RX_RMAX;
    f->lmax = RX_LMAX;

    /* Edges in table order; repeats of a group within a row group are chained. */
    const uint16_t *e = rx_edges[k];
    unsigned n = 0;
    for (unsigned r = 0; r < f->q; ++r) {
        f->layer_first[r] = (uint16_t)n;
        f->layer_count[r] = (uint8_t)e[0];
        unsigned scratch = 0;
        for (unsigned j = 0; j < e[0]; ++j, ++n) {
            rx_edge_t *x = &f->edges[n];
            x->group = (uint8_t)(e[1 + j] >> 9);
            x->shift = e[1 + j] & 511;
            x->home = (uint16_t)(RX_SLOT * x->group);
            x->write = x->home;
            for (unsigned i = n; i-- > f->layer_first[r];) {
                if (f->edges[i].group == x->group) {
                    x->repeat = 1;
                    x->write = (uint16_t)(RX_SLOT * (RX_GROUPS + scratch++));
                    x->previous = f->edges[i].shift;
                    break;
                }
            }
        }
        e += 1 + e[0];
    }
    /* Frames: each group starts an iteration in the frame of its last edge. */
    for (unsigned i = 0; i < f->edge_count; ++i) {
        f->frame[f->edges[i].group] = f->edges[i].shift;
    }
    uint16_t now[RX_GROUPS];
    memcpy(now, f->frame, sizeof now);
    for (unsigned r = 0; r < f->q; ++r) {
        const unsigned first = f->layer_first[r], count = f->layer_count[r];
        for (unsigned j = 0; j < count; ++j) {
            rx_edge_t *x = &f->edges[first + j];
            const unsigned start = (now[x->group] + 360 - x->shift) % 360;
            for (unsigned v = 0; v < RX_VECTORS; ++v) {
                f->reads[(first * RX_VECTORS) + v * count + j] =
                    (uint16_t)(x->home + (start + 16 * v) % 360);
            }
        }
        for (unsigned j = 0; j < count; ++j) {
            now[f->edges[first + j].group] = f->edges[first + j].shift;
        }
    }
    /* Layer 0's edge to parity group q - 1: lane 0 of vector 0 is the missing p_(-1). */
    f->missing_edge = f->layer_count[0] - 1;
    f->missing_read = f->reads[f->missing_edge];

    /* BB scrambling PRBS (5.2.2): 1 + X^14 + X^15 from 100101010000000. */
    unsigned st = 0x00a9;
    for (unsigned i = 0; i < f->kbch / 8; ++i) {
        unsigned byte = 0;
        for (unsigned b = 0; b < 8; ++b) {
            const unsigned bit = ((st >> 13) ^ (st >> 14)) & 1;
            st = ((st << 1) | bit) & 0x7fff;
            byte = (byte << 1) | bit;
        }
        f->prbs[i] = (uint8_t)byte;
    }
    return 0;
}

/* ---------------------------------------------------------------- clean check */

static unsigned popcount8(unsigned x)
{
    x -= (x >> 1) & 0x55;
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 15;
}

/* Failing LDPC parity checks of hard bits: re-encode their BCH word and compare.
 * A parity difference d (natural order) fails check c when d_c ^ d_(c-1) is set. */
static unsigned ldpc_failed(rx_fec_t *f, const uint8_t *hard)
{
    uint8_t *bits = f->bits->bits;
    const unsigned word = f->nbch / 8;
    memcpy(bits, hard, word);
    dvbs2_fast_ldpc(f->encoder, f->bits);
    unsigned failed = 0;
    if (memcmp(bits + word, hard + word, RX_SHORT_BITS / 8 - word)) {
        unsigned previous = 0;
        for (unsigned i = word; i < RX_SHORT_BITS / 8; ++i) {
            const unsigned d = bits[i] ^ hard[i];
            failed += popcount8(d ^ (d >> 1) ^ (previous << 7));
            previous = d & 1;
        }
    }
    return failed;
}

/* BCH parity of the hard bits matches their re-encoded BBFRAME. ldpc_failed has
 * already copied the BBFRAME if copied is set. */
static int bch_ok(rx_fec_t *f, const uint8_t *hard, int copied)
{
    uint8_t *bits = f->bits->bits;
    const unsigned info = f->kbch / 8;
    if (!copied) {
        memcpy(bits, hard, info);
    }
    dvbs2_fast_bch(f->encoder, f->bits);
    return !memcmp(bits + info, hard + info, f->nbch / 8 - info);
}

/* CRC-8 of 5.1.4: polynomial 0xd5, MSB first. */
static uint8_t crc8(const uint8_t *p, unsigned n)
{
    unsigned c = 0;
    for (unsigned i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned b = 0; b < 8; ++b) {
            c = ((c << 1) ^ (c & 0x80 ? 0xd5 : 0)) & 0xff;
        }
    }
    return (uint8_t)c;
}

/* Descramble the BBHEADER in place and check it (GCS, single stream, UPL = 0,
 * SYNCD = 0, byte-aligned DFL that fits); then descramble the data field.
 * Returns DFL in bytes or -1. */
static int bbframe(const rx_fec_t *f, uint8_t *hard)
{
    for (unsigned i = 0; i < 10; ++i) {
        hard[i] ^= f->prbs[i];
    }
    const unsigned dfl = (unsigned)hard[4] << 8 | hard[5];
    if (crc8(hard, 9) != hard[9] || (hard[0] & 0xe0) != 0x60 || hard[2] || hard[3] ||
        hard[7] || hard[8] || dfl % 8 || dfl > f->kbch - 80u) {
        return -1;
    }
    for (unsigned i = 10; i < 10 + dfl / 8; ++i) {
        hard[i] ^= f->prbs[i];
    }
    return (int)(dfl / 8);
}

/* ---------------------------------------------------------------- decoder */

static int8_t sat8(int x) { return (int8_t)(x < -128 ? -128 : x > 127 ? 127 : x); }
static int abs8(int x) { return x < 0 ? (x == -128 ? 127 : -x) : x; }

/* Minimum magnitude to message magnitude: (m * scale) >> 7, minus offset, at least 0. */
static int norm(const rx_fec_t *f, int m)
{
    const int n = ((m * f->scale) >> 7) - f->offset;
    return n < 0 ? 0 : n > f->rmax ? f->rmax : n;
}

/* Pass 1 of one vector: q = window - R for every edge, the two smallest |q|, and
 * the sign parities of q and of the beliefs read (the check as seen right now). */
static void pass1_c(rx_fec_t *f, const uint16_t *reads, const int8_t *r, int8_t *qtmp,
                    unsigned count, int8_t state[64])
{
    for (unsigned lane = 0; lane < 16; ++lane) {
        int m1 = 127, m2 = 127, s = 0, sb = 0;
        for (unsigned j = 0; j < count; ++j) {
            const int b = f->belief[reads[j] + lane];
            const int q = sat8(b - r[16 * j + lane]);
            const int a = abs8(q);
            qtmp[16 * j + lane] = (int8_t)q;
            m2 = m2 < (m1 > a ? m1 : a) ? m2 : (m1 > a ? m1 : a);
            m1 = m1 < a ? m1 : a;
            s ^= q;
            sb ^= b;
        }
        state[lane] = (int8_t)m1;
        state[16 + lane] = (int8_t)m2;
        state[32 + lane] = (int8_t)s;
        state[48 + lane] = (int8_t)sb;
    }
}

/* Pass 2 of one vector: new messages and beliefs, written aligned. */
static void pass2_c(rx_fec_t *f, const rx_edge_t *edges, int8_t *r, const int8_t *qtmp,
                    unsigned count, unsigned v, const int8_t state[64])
{
    for (unsigned lane = 0; lane < 16; ++lane) {
        const int m2 = state[16 + lane], neg = state[32 + lane] < 0;
        const int n1 = norm(f, state[lane]), n2 = norm(f, m2);
        const int s1 = neg ? -n1 : n1, s2 = neg ? -n2 : n2;
        for (unsigned j = 0; j < count; ++j) {
            const int q = qtmp[16 * j + lane];
            const int sel = abs8(q) < m2 ? s2 : s1;
            const int msg = q < 0 ? -sel : sel;
            int8_t *out = f->belief + edges[j].write + 16 * v + lane;
            *out = edges[j].repeat ? sat8(msg - r[16 * j + lane]) : sat8(q + msg);
            r[16 * j + lane] = (int8_t)msg;
        }
    }
}

/* Positions 360..375 repeat 0..15. */
static void tail(int8_t *slot)
{
    memcpy(slot + 360, slot, 16);
}

/* Slot (frame 'to') = slot read at the previous frame + scratch, then back home. */
static void repeat_c(rx_fec_t *f, const rx_edge_t *x)
{
    int8_t *home = f->belief + x->home, *scratch = f->belief + x->write;
    const unsigned start = (x->previous + 360 - x->shift) % 360;
    for (unsigned t = 0; t < 360; ++t) {
        scratch[t] = sat8(home[(start + t) % 360] + scratch[t]);
    }
    memcpy(home, scratch, 360);
    tail(home);
}

/* One layer. Returns how many of its checks the beliefs read in pass 1 failed. */
static unsigned layer(rx_fec_t *f, unsigned L)
{
    const unsigned first = f->layer_first[L], count = f->layer_count[L];
    const rx_edge_t *edges = f->edges + first;
    const uint16_t *reads = f->reads + first * RX_VECTORS;
    int8_t *r = f->r + 16 * first * RX_VECTORS;
    int8_t *missing = NULL, saved = 0;
    unsigned odd = 0;
    if (L == 0) {
        missing = f->belief + f->missing_read;
        saved = *missing;
        *missing = 127;
        r[16 * f->missing_edge] = 0;
    }
    for (unsigned v = 0; v < RX_VECTORS; ++v) {
        pass1_c(f, reads + v * count, r + 16 * v * count, f->qtmp + 16 * v * count, count,
                f->state + 64 * v);
    }
    if (missing) {
        *missing = saved;
    }
    for (unsigned v = 0; v < RX_VECTORS; ++v) {
        const int8_t *s = f->state + 64 * v;
        for (unsigned lane = 0; lane < (v == RX_VECTORS - 1 ? 8u : 16u); ++lane) {
            odd += s[48 + lane] < 0;
        }
        pass2_c(f, edges, r + 16 * v * count, f->qtmp + 16 * v * count, count, v, s);
    }
    if (missing) {
        /* Check 0's lane wrote p_(M-1) at position 0 of the new frame. */
        f->belief[edges[f->missing_edge].home] = saved;
        r[16 * f->missing_edge] = 0;
    }
    for (unsigned j = 0; j < count; ++j) {
        if (!edges[j].repeat) {
            tail(f->belief + edges[j].home);
        }
    }
    for (unsigned j = 0; j < count; ++j) {
        if (edges[j].repeat) {
            repeat_c(f, &edges[j]);
        }
    }
    return odd;
}

/* Gain for the channel LLRs in sixteenths, aiming at RX_UNITS integer units per
 * natural LLR whatever the demodulator's scale K. If x / K ~ N(+-1, s^2), the natural
 * LLR is 2 x / (K s^2) = 2 m x / v with m and v the mean and variance of |x| (|x|
 * rarely folds over zero above about 1 dB Es/N0). Never more than 4. */
static unsigned gain16(const int8_t *llr, unsigned n)
{
    uint64_t s1 = 0, s2 = 0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned a = (unsigned)abs8(llr[i]);
        s1 += a;
        s2 += a * a;
    }
    const uint64_t den = n * s2 - s1 * s1;
    if (!den) {
        return 64;
    }
    const uint64_t g = (32u * RX_UNITS * s1 * n + den / 2) / den;
    return g < 1 ? 1 : g > 64 ? 64 : (unsigned)g;
}

/* Channel LLRs into the slots, each in its starting frame, scaled, rounded
 * symmetrically and clipped to +-RX_LMAX. */
static unsigned load(rx_fec_t *f, const int8_t *llr)
{
    const unsigned groups = f->groups, q = f->q, gain = gain16(llr, RX_SHORT_BITS);
    for (unsigned g = 0; g < RX_GROUPS; ++g) {
        int8_t *slot = f->belief + RX_SLOT * g;
        const unsigned frame = f->frame[g];
        for (unsigned t = 0; t < 360; ++t) {
            const unsigned bit = g < groups ? 360 * g + t : f->nbch + (g - groups) + q * t;
            const int x = llr[bit], a = (int)((abs8(x) * gain + 8) >> 4);
            slot[(t + frame) % 360] = (int8_t)((x < 0 ? -1 : 1) * (a > f->lmax ? f->lmax : a));
        }
        tail(slot);
    }
    memset(f->r, 0, 16 * f->edge_count * RX_VECTORS);
    return gain;
}

/* Every check satisfied by the signs of the current beliefs. */
static int syndrome_ok(const rx_fec_t *f)
{
    for (unsigned L = 0; L < f->q; ++L) {
        int8_t x[360] = {0};
        for (unsigned j = 0; j < f->layer_count[L]; ++j) {
            const rx_edge_t *e = &f->edges[f->layer_first[L] + j];
            const int8_t *slot = f->belief + e->home;
            const unsigned start = (f->frame[e->group] + 360 - e->shift) % 360;
            for (unsigned t = L == 0 && j == f->missing_edge; t < 360; ++t) {
                x[t] ^= slot[(start + t) % 360];
            }
        }
        for (unsigned t = 0; t < 360; ++t) {
            if (x[t] < 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* Signs of the information groups as packed hard bits, MSB first. */
static void hard_info(const rx_fec_t *f, uint8_t *hard)
{
    for (unsigned g = 0; g < f->groups; ++g) {
        const int8_t *slot = f->belief + RX_SLOT * g;
        const unsigned frame = f->frame[g];
        for (unsigned i = 0; i < 45; ++i) {
            unsigned byte = 0;
            for (unsigned b = 0; b < 8; ++b) {
                byte = (byte << 1) | (slot[(8 * i + b + frame) % 360] < 0);
            }
            hard[45 * g + i] = (uint8_t)byte;
        }
    }
}

/* ---------------------------------------------------------------- frame */

int rx_fec_frame(rx_fec_t *f, rx_frame_t *frame, const uint8_t **payload,
                 rx_fec_stats_t *stats)
{
    rx_fec_stats_t s = {0};
    int bytes = -1;
    if (!f || !frame || !payload || frame->modcod != f->modcod || !(frame->type & 2)) {
        goto out;
    }
    s.failed_checks = (uint16_t)ldpc_failed(f, frame->hard);
    if (s.failed_checks) {
        load(f, frame->llr);
        while (s.iterations < f->max_iterations) {
            unsigned odd = 0;
            ++s.iterations;
            for (unsigned L = 0; L < f->q; ++L) {
                odd += layer(f, L);
            }
            /* The layers saw satisfied checks, or this is the last chance. */
            if ((!odd || s.iterations == f->max_iterations) && syndrome_ok(f)) {
                hard_info(f, frame->hard);
                s.ldpc_ok = 1;
                break;
            }
        }
        if (!s.ldpc_ok) {
            goto out;
        }
    }
    s.ldpc_ok = 1;
    s.bch_ok = (uint8_t)bch_ok(f, frame->hard, !s.iterations);
    if (s.bch_ok) {
        bytes = bbframe(f, frame->hard);
        s.header_ok = bytes >= 0;
        *payload = frame->hard + 10;
    }
out:
    if (stats) {
        *stats = s;
    }
    return bytes;
}
