/* Short-frame FEC for the receiver: check the hard bits, decode only if needed.
 *
 * Clean path. The hard bits are checked as received: the LDPC syndrome in
 * quasi-cyclic form on packed words, the BCH remainder, then BB descrambling of
 * the BBHEADER, its CRC-8 and the data field. Nothing reads the LLRs.
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
#include "rx_fec_internal.h"
#include "rx_fec_tables.h"

#include <string.h>

#define FIRST_MODCOD 4
#define LAST_MODCOD 10
#define MAX_ITERATIONS 30

static unsigned rate_index(unsigned modcod) { return modcod - FIRST_MODCOD; }

static size_t round16(size_t n) { return (n + 15) & ~(size_t)15; }

/* Layout of the workspace after the header, in bytes, for one MODCOD. */
typedef struct {
    size_t belief, r, qtmp, reads, edges, prbs, rows, total;
} layout_t;

/* Edges in all row groups; the largest row group and most repeated groups in one. */
static unsigned count_edges(unsigned k, unsigned *most, unsigned *repeats)
{
    const uint16_t *e = rx_edges[k];
    unsigned total = 0;
    *most = *repeats = 0;
    for (unsigned r = 0; r < rx_q[k]; ++r, e += 1 + e[0]) {
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

static layout_t layout(unsigned k)
{
    unsigned most, repeats;
    const unsigned edges = count_edges(k, &most, &repeats);
    layout_t l;
    l.belief = round16(sizeof(rx_fec_t));
    l.r = l.belief + (size_t)RX_SLOT * (RX_GROUPS + repeats);
    l.qtmp = l.r + (size_t)edges * RX_VECTORS * 16;
    l.reads = l.qtmp + (size_t)most * RX_VECTORS * 16;
    l.edges = l.reads + round16((size_t)edges * RX_VECTORS * 2);
    l.prbs = l.edges + round16(edges * sizeof(rx_edge_t));
    l.rows = l.prbs + round16(rx_kbch[k] / 8);
    l.total = l.rows + 4 * (RX_ROW_WORDS * rx_q[k] + RX_TWICE_WORDS * rx_nbch[k] / 360);
    return l;
}

size_t rx_fec_size(unsigned modcod)
{
    if (modcod < FIRST_MODCOD || modcod > LAST_MODCOD) {
        return 0;
    }
    return layout(rate_index(modcod)).total;
}

/* ---------------------------------------------------------------- init */

int rx_fec_init(rx_fec_t *f, unsigned modcod)
{
    if (!f || ((uintptr_t)f & 15) || !rx_fec_size(modcod)) {
        return -1;
    }
    const unsigned k = rate_index(modcod);
    const layout_t l = layout(k);
    memset(f, 0, l.total);
    uint8_t *base = (uint8_t *)f;
    f->modcod = modcod;
    f->kbch = rx_kbch[k];
    f->nbch = rx_nbch[k];
    f->q = rx_q[k];
    f->groups = rx_nbch[k] / 360;
    unsigned repeats;
    f->edge_count = count_edges(k, &f->most, &repeats);
    f->belief = (int8_t *)(base + l.belief);
    f->r = (int8_t *)(base + l.r);
    f->qtmp = (int8_t *)(base + l.qtmp);
    f->reads = (uint16_t *)(base + l.reads);
    f->edges = (rx_edge_t *)(base + l.edges);
    f->prbs = base + l.prbs;
    f->rows = (uint32_t *)(base + l.rows);
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

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* 32 bits starting at bit 'bit' of big-endian words w (bit 0 = MSB of w[0]). */
static uint32_t window32(const uint32_t *w, unsigned bit)
{
    const unsigned i = bit / 32, s = bit % 32;
    return s ? (w[i] << s) | (w[i + 1] >> (32 - s)) : w[i];
}

/* Hacker's Delight transpose: afterwards bit 31 - j of a[i] is the old bit 31 - i of a[j]. */
static void transpose32(uint32_t a[32])
{
    uint32_t m = 0x0000ffffu;
    for (unsigned j = 16; j; j >>= 1, m ^= m << j) {
        for (unsigned k = 0; k < 32; k = (k + j + 1) & ~j) {
            const uint32_t t = (a[k] ^ (a[k + j] >> j)) & m;
            a[k] ^= t;
            a[k + j] ^= t << j;
        }
    }
}

static unsigned popcount32(uint32_t x)
{
    x -= (x >> 1) & 0x55555555u;
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0f0f0f0fu;
    return (x * 0x01010101u) >> 24;
}

/* Number of failing parity checks on packed hard bits. Row group r of the
 * information part is A_r = sum of the information groups rotated by their shifts,
 * as 12 big-endian words (bit t = check (r, t)). The parity part of check c is
 * p_c ^ p_(c-1), a stream in natural order c = r + q t: cut it into 360 columns of q
 * bits and transpose 32 columns at a time into the same row form. */
static unsigned ldpc_syndrome(rx_fec_t *f, const uint8_t *hard)
{
    uint32_t *rows = f->rows, *twice = rows + RX_ROW_WORDS * f->q;
    const unsigned q = f->q;
    memset(rows, 0, 4 * RX_ROW_WORDS * q);
    /* Each information group doubled (720 bits), so a rotation is one window. */
    for (unsigned g = 0; g < f->groups; ++g) {
        uint8_t bytes[96] = {0};
        memcpy(bytes, hard + 45 * g, 45);
        memcpy(bytes + 45, hard + 45 * g, 45);
        memcpy(bytes + 90, hard + 45 * g, 6);
        for (unsigned w = 0; w < RX_TWICE_WORDS; ++w) {
            twice[RX_TWICE_WORDS * g + w] = be32(bytes + 4 * w);
        }
    }
    for (unsigned i = 0; i < f->edge_count; ++i) {
        const rx_edge_t *x = &f->edges[i];
        if (x->group >= f->groups) {
            continue;
        }
        unsigned r = 0;
        while (i >= (unsigned)f->layer_first[r] + f->layer_count[r]) {
            ++r;
        }
        const uint32_t *u = twice + RX_TWICE_WORDS * x->group;
        for (unsigned w = 0; w < 12; ++w) {
            rows[RX_ROW_WORDS * r + w] ^= window32(u, 360 - x->shift + 32 * w);
        }
    }
    /* d_c = p_c ^ p_(c-1) on the packed parity stream (p_(-1) = 0), as 32-bit columns. */
    const uint8_t *p = hard + f->nbch / 8;
    const unsigned parity_bits = 360 * q;
    uint32_t prev = 0, a[32];
    for (unsigned w = 0; w < 12; ++w) {
        for (unsigned j = 0; j < 32; ++j) {
            const unsigned t = 32 * w + j;
            uint32_t col = 0;
            if (t < 360) {
                /* Bits q t .. q t + q - 1 of d, MSB first; q <= 25 fits a window of 32. */
                const unsigned bit = q * t;
                const uint8_t *b = p + bit / 8;
                uint64_t x = 0;
                for (unsigned k = 0; k < 5; ++k) {
                    x = (x << 8) | (bit / 8 + k < (parity_bits + 7) / 8 ? b[k] : 0);
                }
                const uint32_t bits = (uint32_t)(x >> (8 - bit % 8));
                const uint32_t mask = ~0u << (32 - q);
                col = (bits ^ ((bits >> 1) | (prev << 31))) & mask;
                prev = (bits >> (32 - q)) & 1;
            }
            a[j] = col;
        }
        transpose32(a);
        for (unsigned r = 0; r < q; ++r) {
            rows[RX_ROW_WORDS * r + w] ^= a[r];
        }
    }
    unsigned failed = 0;
    for (unsigned r = 0; r < q; ++r) {
        rows[RX_ROW_WORDS * r + 11] &= 0xff000000u;    /* bits 352..359 */
        for (unsigned w = 0; w < 12; ++w) {
            failed += popcount32(rows[RX_ROW_WORDS * r + w]);
        }
    }
    return failed;
}

/* Remainder of the Nbch-bit BCH word mod g(x) is zero. Byte-wise division, the
 * 168-bit register as 21 bytes. */
static int bch_ok(const rx_fec_t *f, const uint8_t *hard)
{
    uint8_t reg[21] = {0};
    for (unsigned i = 0; i < f->nbch / 8; ++i) {
        const uint8_t *t = rx_bch_table + 21 * (reg[0] ^ hard[i]);
        for (unsigned j = 0; j < 20; ++j) {
            reg[j] = reg[j + 1] ^ t[j];
        }
        reg[20] = t[20];
    }
    uint8_t any = 0;
    for (unsigned j = 0; j < 21; ++j) {
        any |= reg[j];
    }
    return !any;
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
    s.failed_checks = (uint16_t)ldpc_syndrome(f, frame->hard);
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
    s.bch_ok = (uint8_t)bch_ok(f, frame->hard);
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
