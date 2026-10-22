/* Short-frame FEC for the receiver: check the hard bits, decode only if needed.
 *
 * Clean path. The received information bits are re-encoded with the transmitter's
 * fast encoder (components/dvbs2, PIE on the S3) and both parity fields compared;
 * then the BBHEADER is descrambled and its CRC-8 and fields checked, and the data
 * field descrambled in place. Nothing reads the LLRs.
 *
 * Decoder. Layered normalized min-sum with int8 beliefs and messages (EN 302 307-1
 * Annex C codes; rx_fec_tables.h has the derivation). A row group of q is a layer of
 * 360 independent checks, held as 23 vectors of 16 lanes (lanes 360..367 are padding:
 * they compute garbage that is never read back). Every edge of a layer is a column
 * group read at a cyclic shift. Each group's 360 beliefs live in one 384-byte slot in
 * the frame of the last edge that wrote it (position p holds bit p - frame), with
 * positions 0..15 repeated at 360..375 so any 16-byte window is contiguous. Pass 1
 * reads every window of the layer, pass 2 writes each group back aligned in its
 * edge's frame, so loads may be unaligned but stores never are. All frames repeat
 * every iteration and are fixed at init.
 *
 * A group can appear up to three times in one row group (3/5, 5/6). Its first edge
 * writes the slot; each repeat stores its message change into a scratch slot, which
 * is then added to the slot read at the relative shift.
 * Check 0 has no p_(-1): layer 0's edge to parity group q - 1 has one lane, check 0,
 * that would read p_(M-1). That lane sees +127 with message 0, so it never sets a
 * minimum or a sign, and p_(M-1) is restored after the layer.
 *
 * Quantization (tests/test_rx_fec.py measures it): channel LLRs are rescaled per
 * frame to about RX_UNITS units per natural LLR and clipped at RX_LMAX; messages are
 * 112/128 of the minimum, at most RX_RMAX. The clip below RX_RMAX lets checks overturn
 * confidently wrong bits, which a high-SNR frame hit by a glitch has; it costs nothing
 * in AWGN.
 *
 * Stopping: after at most MAX_ITERATIONS, or from the third iteration when the checks
 * failing on the beliefs pass 1 read are no fewer than two iterations before, or more
 * than a sixth of all checks (hopeless frames stay near half). */
#include "dvbs2rx.h"
#include "dvbs2.h"
#include "rx_fec_internal.h"
#include "rx_fec_tables.h"

#include <string.h>

#define FIRST_MODCOD 4
#define LAST_MODCOD 10
#define MAX_ITERATIONS 8   /* a damaged frame on a good link needs 2 to 5 */

/* Functions of the frame path keep their own sections, so linker.lf can place them in
 * IRAM by name. */
#ifdef ESP_PLATFORM
#define HOT __attribute__((noinline, noclone))
#else
#define HOT
#endif

static size_t round16(size_t n) { return (n + 15) & ~(size_t)15; }

static const dvbs2_mode *short_mode(unsigned modcod)
{
    return modcod >= FIRST_MODCOD && modcod <= LAST_MODCOD ? dvbs2_find_mode_short(modcod) : NULL;
}

/* Edges in all row groups; the largest row group and most repeated groups in one. */
static unsigned count_edges(const uint16_t *e, unsigned q, unsigned *most, unsigned *repeats)
{
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

/* Byte offsets of the two areas. The internal part holds what the clean path touches:
 * the header, the BB PRBS, the encoder context with its resident BCH table and the
 * encoder's 2 KB workspace. The work area holds the decoder. */
typedef struct {
    size_t prbs, encoder, tail, bits, total;
    size_t belief, r, qtmp, reads, writes, checks, edges, state, work;
} layout_t;

static layout_t layout(const dvbs2_mode *mode)
{
    unsigned most, repeats;
    const unsigned edges = count_edges(rx_edges[mode->modcod - FIRST_MODCOD], mode->q, &most,
                                       &repeats);
    layout_t l;
    l.prbs = round16(sizeof(rx_fec_t));
    l.encoder = l.prbs + round16(mode->kbch / 8);
    l.tail = l.encoder + round16(dvbs2_fast_mode_size(mode));
    l.bits = l.tail + RX_ENCODER_TAIL;
    l.total = l.bits + DVBS2_SHORT_FAST_WORKSPACE_USED;
    l.belief = 0;
    l.r = l.belief + (size_t)RX_SLOT * (RX_GROUPS + repeats);
    l.qtmp = l.r + (size_t)(edges * RX_VECTORS + 1) * 16;
    l.reads = l.qtmp + (size_t)(most * RX_VECTORS + 1) * 32;
    l.writes = l.reads + round16((size_t)(edges * RX_VECTORS + 1) * 2);
    l.checks = l.writes + round16(edges * 2);
    l.edges = l.checks + round16(edges * 4);
    l.state = l.edges + round16(edges * sizeof(rx_edge_t));
    l.work = l.state + RX_VECTORS * 64 + 16;
    return l;
}

size_t rx_fec_size(unsigned modcod)
{
    const dvbs2_mode *mode = short_mode(modcod);
    return mode ? layout(mode).total : 0;
}

size_t rx_fec_work_size(unsigned modcod)
{
    const dvbs2_mode *mode = short_mode(modcod);
    return mode ? layout(mode).work : 0;
}

/* ---------------------------------------------------------------- init */

/* Row group r's edges from the table: first occurrences, then repeats, each in table
 * order. A repeat remembers the shift of the group's previous edge in table order. */
static void order_edges(rx_fec_t *f, const uint16_t *e, unsigned r, unsigned n)
{
    rx_edge_t *out = f->edges + n;
    unsigned repeats = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned j = 0; j < e[0]; ++j) {
            const unsigned group = e[1 + j] >> 9;
            int previous = -1;
            for (unsigned i = 0; i < j; ++i) {
                if (e[1 + i] >> 9 == group) {
                    previous = e[1 + i] & 511;
                }
            }
            if ((previous >= 0) != (int)pass) {
                continue;
            }
            rx_edge_t *x = out++;
            x->group = (uint8_t)group;
            x->shift = e[1 + j] & 511;
            x->home = (uint16_t)(RX_SLOT * group);
            x->repeat = (uint8_t)pass;
            x->previous = (uint16_t)(previous < 0 ? 0 : previous);
            f->writes[x - f->edges] =
                pass ? (uint16_t)(RX_SLOT * (RX_GROUPS + repeats++)) : x->home;
        }
    }
    f->layer_first[r] = (uint16_t)n;
    f->layer_count[r] = (uint8_t)e[0];
    f->layer_repeats[r] = (uint8_t)repeats;
}

int rx_fec_init(rx_fec_t *f, unsigned modcod, void *work)
{
    const dvbs2_mode *mode = short_mode(modcod);
    if (!f || ((uintptr_t)f & 15) || ((uintptr_t)work & 15) || !mode) {
        return -1;
    }
    const layout_t l = layout(mode);
    memset(f, 0, l.total);
    uint8_t *base = (uint8_t *)f;
    const uint16_t *table = rx_edges[modcod - FIRST_MODCOD];
    unsigned repeats;
    f->modcod = modcod;
    f->kbch = mode->kbch;
    f->nbch = mode->nbch;
    f->q = mode->q;
    f->groups = mode->nbch / 360;
    f->edge_count = count_edges(table, f->q, &f->most, &repeats);
    f->max_iterations = MAX_ITERATIONS;
    f->prbs = base + l.prbs;
    f->encoder = (dvbs2_fast *)(base + l.encoder);
    f->bits = (dvbs2_workspace *)(base + l.bits);
    /* The LDPC program, columns and CRC stay resident (1.6 KB); the 8 KB BCH table is
     * read from flash: core 0 has the data cache to itself and keeps it warm (31.6k
     * cycles a clean frame either way, 155k after a full cache flush). */
    if (dvbs2_fast_prepare(f->encoder, mode, 1, DVBS2_PIE_BCH | DVBS2_PIE_LDPC) ||
        dvbs2_fast_cache_tail_size(f->encoder) > RX_ENCODER_TAIL ||
        dvbs2_fast_cache_split(f->encoder, NULL, base + l.tail)) {
        return -1;
    }
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
    for (unsigned v = 0; v < 256; ++v) {
        unsigned c = v;
        for (unsigned b = 0; b < 8; ++b) {
            c = ((c << 1) ^ (c & 0x80 ? 0xd5 : 0)) & 0xff;
        }
        f->crc8[v] = (uint8_t)c;
    }
    if (!work) {
        return 0;
    }
    uint8_t *w = work;
    memset(w, 0, l.work);
    f->belief = (int8_t *)(w + l.belief);
    f->r = (int8_t *)(w + l.r);
    f->qtmp = (int8_t *)(w + l.qtmp);
    f->reads = (uint16_t *)(w + l.reads);
    f->writes = (uint16_t *)(w + l.writes);
    f->checks = (uint16_t *)(w + l.checks);
    f->edges = (rx_edge_t *)(w + l.edges);
    f->state = (int8_t *)(w + l.state);
#ifdef DVBS2RX_PIE
    f->pie = 1;
#endif
    for (unsigned r = 0, n = 0; r < f->q; n += table[0], table += 1 + table[0], ++r) {
        order_edges(f, table, r, n);
    }
    /* Frames: each group starts an iteration in the frame of its last edge, the last
     * repeat of the last row group that has it. */
    for (unsigned i = 0; i < f->edge_count; ++i) {
        f->frame[f->edges[i].group] = f->edges[i].shift;
    }
    uint16_t now[RX_GROUPS];
    memcpy(now, f->frame, sizeof now);
    for (unsigned r = 0; r < f->q; ++r) {
        const unsigned first = f->layer_first[r], count = f->layer_count[r];
        for (unsigned j = 0; j < count; ++j) {
            const rx_edge_t *x = &f->edges[first + j];
            const unsigned start = (now[x->group] + 360 - x->shift) % 360;
            for (unsigned v = 0; v < RX_VECTORS; ++v) {
                f->reads[first * RX_VECTORS + v * count + j] =
                    (uint16_t)(x->home + (start + 16 * v) % 360);
            }
            f->checks[2 * (first + j)] = x->home;
            f->checks[2 * (first + j) + 1] =
                (uint16_t)((f->frame[x->group] + 360 - x->shift) % 360);
        }
        for (unsigned j = 0; j < count; ++j) {
            now[f->edges[first + j].group] = f->edges[first + j].shift;
        }
    }
    /* Layer 0's edge to parity group q - 1, its last first occurrence: lane 0 of vector
     * 0 is the missing p_(-1). It reads position 359, which has no copy in the tail. */
    f->missing_edge = f->layer_count[0] - f->layer_repeats[0] - 1;
    f->missing_read = f->reads[f->missing_edge];
    return 0;
}

/* ---------------------------------------------------------------- checks */

#ifdef DVBS2RX_PIE
#define KERNEL(f, name) ((f)->pie ? name##_pie : name)
#else
#define KERNEL(f, name) ((void)(f), name)
#endif


static inline unsigned popcount8(unsigned x)
{
    x -= (x >> 1) & 0x55;
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 15;
}

/* Nonzero if n bytes differ: a word at a time when a and b share their alignment
 * (the ROM memcmp takes 13 cycles a byte). */
static HOT int differ(const uint8_t *a, const uint8_t *b, unsigned n)
{
    uint32_t d = 0;
    unsigned i = 0;
    if (!(((uintptr_t)a ^ (uintptr_t)b) & 3)) {
        for (; i < n && ((uintptr_t)(a + i) & 3); ++i) {
            d |= a[i] ^ b[i];
        }
        for (; i + 4 <= n; i += 4) {
            d |= *(const uint32_t *)(const void *)(a + i) ^ *(const uint32_t *)(const void *)(b + i);
        }
    }
    for (; i < n; ++i) {
        d |= a[i] ^ b[i];
    }
    return d != 0;
}

/* Failing LDPC parity checks of hard bits: re-encode their BCH word and compare.
 * A parity difference d (natural order) fails check c when d_c ^ d_(c-1) is set. */
static HOT unsigned ldpc_failed(rx_fec_t *f, const uint8_t *hard)
{
    uint8_t *bits = f->bits->bits;
    const unsigned word = f->nbch / 8;
    memcpy(bits, hard, word);
    dvbs2_fast_ldpc(f->encoder, f->bits);
    unsigned failed = 0;
    if (differ(bits + word, hard + word, RX_SHORT_BITS / 8 - word)) {
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
static HOT int bch_ok(rx_fec_t *f, const uint8_t *hard, int copied)
{
    uint8_t *bits = f->bits->bits;
    const unsigned info = f->kbch / 8;
    if (!copied) {
        memcpy(bits, hard, info);
    }
    dvbs2_fast_bch(f->encoder, f->bits);
    return !differ(bits + info, hard + info, f->nbch / 8 - info);
}

/* CRC-8 of 5.1.4 by table: polynomial 0xd5, MSB first. */
static HOT uint8_t crc8(const rx_fec_t *f, const uint8_t *p, unsigned n)
{
    unsigned c = 0;
    for (unsigned i = 0; i < n; ++i) {
        c = f->crc8[c ^ p[i]];
    }
    return (uint8_t)c;
}

/* x ^= prbs over n bytes; x and prbs share their alignment modulo 16. */
static HOT void descramble(const rx_fec_t *f, uint8_t *x, const uint8_t *prbs, unsigned n)
{
    unsigned i = 0;
    for (; i < n && ((uintptr_t)(x + i) & 15); ++i) {
        x[i] ^= prbs[i];
    }
    if (!(((uintptr_t)x ^ (uintptr_t)prbs) & 15) && n - i >= 16) {
        KERNEL(f, rx_xor)(x + i, prbs + i, (n - i) / 16);
        i += (n - i) & ~15u;
    }
    for (; i < n; ++i) {
        x[i] ^= prbs[i];
    }
}

/* Descramble the BBHEADER in place and check it (GCS, single stream, UPL = 0,
 * SYNCD = 0, byte-aligned DFL that fits); then descramble the data field.
 * Returns DFL in bytes or -1. */
static HOT int bbframe(const rx_fec_t *f, uint8_t *hard)
{
    descramble(f, hard, f->prbs, 10);
    const unsigned dfl = (unsigned)hard[4] << 8 | hard[5];
    if (crc8(f, hard, 9) != hard[9] || (hard[0] & 0xe0) != 0x60 || hard[2] || hard[3] ||
        hard[7] || hard[8] || dfl % 8 || dfl > f->kbch - 80u) {
        return -1;
    }
    descramble(f, hard + 10, f->prbs + 10, dfl / 8);
    return (int)(dfl / 8);
}

/* ---------------------------------------------------------------- kernels */

static int sat8(int x) { return x < -128 ? -128 : x > 127 ? 127 : x; }
static int abs8(int x) { return x < 0 ? (x == -128 ? 127 : -x) : x; }
static int norm(int m) { const int n = (m * RX_SCALE) >> 7; return n > RX_RMAX ? RX_RMAX : n; }

void rx_pass1(const int8_t *belief, const uint16_t *reads, const int8_t *r, int8_t *qtmp,
              int8_t *state, unsigned count)
{
    int8_t *failed = state + 64 * RX_VECTORS;
    memset(failed, 0, 16);
    for (unsigned v = 0; v < RX_VECTORS; ++v, reads += count, r += 16 * count, state += 64) {
        for (unsigned lane = 0; lane < 16; ++lane) {
            int m1 = 127, m2 = 127, s = 0, sb = 0;
            for (unsigned j = 0; j < count; ++j) {
                const int b = belief[reads[j] + lane], q = sat8(b - r[16 * j + lane]);
                const int a = abs8(q);
                qtmp[32 * j + lane] = (int8_t)a;
                qtmp[32 * j + 16 + lane] = (int8_t)q;
                m2 = m2 < (m1 > a ? m1 : a) ? m2 : (m1 > a ? m1 : a);
                m1 = m1 < a ? m1 : a;
                s ^= q;
                sb ^= b;
            }
            state[lane] = (int8_t)m1;
            state[16 + lane] = (int8_t)m2;
            state[32 + lane] = (int8_t)s;
            state[48 + lane] = (int8_t)sb;
            failed[lane] = (int8_t)(failed[lane] + (sb < 0));
        }
        qtmp += 32 * count;
    }
}

void rx_pass2(int8_t *belief, const uint16_t *writes, int8_t *r, const int8_t *qtmp,
              const int8_t *state, unsigned edges)
{
    const unsigned first = edges & 255, count = first + (edges >> 8);
    for (unsigned v = 0; v < RX_VECTORS; ++v, r += 16 * count, qtmp += 32 * count, state += 64) {
        for (unsigned lane = 0; lane < 16; ++lane) {
            const int m2 = state[16 + lane], neg = state[32 + lane] < 0;
            const int n1 = norm(state[lane]), n2 = norm(m2);
            const int s1 = neg ? -n1 : n1, s2 = neg ? -n2 : n2;
            for (unsigned j = 0; j < count; ++j) {
                const int a = qtmp[32 * j + lane], q = qtmp[32 * j + 16 + lane];
                const int sel = a < m2 ? s2 : s1, msg = q < 0 ? -sel : sel;
                belief[writes[j] + 16 * v + lane] =
                    (int8_t)(j < first ? sat8(q + msg) : sat8(msg - r[16 * j + lane]));
                r[16 * j + lane] = (int8_t)msg;
            }
        }
    }
}

void rx_xor(uint8_t *x, const uint8_t *prbs, unsigned vectors)
{
    for (unsigned i = 0; i < 16 * vectors; ++i) {
        x[i] ^= prbs[i];
    }
}

void rx_tails(int8_t *belief, const uint16_t *writes, unsigned count)
{
    for (unsigned j = 0; j < count; ++j) {
        memcpy(belief + writes[j] + 360, belief + writes[j], 16);
    }
}

void rx_repeat(int8_t *home, int8_t *scratch, unsigned start)
{
    for (unsigned t = 0; t < 16 * RX_VECTORS; ++t) {
        scratch[t] = (int8_t)sat8(home[(start + t - t % 16) % 360 + t % 16] + scratch[t]);
    }
    memcpy(home, scratch, 16 * RX_VECTORS);
    memcpy(home + 360, home, 16);
}

void rx_scale(int8_t *belief, unsigned slots, unsigned gain)
{
    for (unsigned i = 0; i < slots * RX_SLOT; ++i) {
        const int x = belief[i], a = (abs8(x) * (int)gain) >> 4;
        belief[i] = (int8_t)((x < 0 ? -1 : 1) * (a > RX_LMAX ? RX_LMAX : a));
    }
}

void rx_moments(const int8_t *llr, unsigned vectors, uint32_t sums[2])
{
    uint32_t s1 = 0, s2 = 0;
    for (unsigned i = 0; i < 16 * vectors; ++i) {
        const uint32_t a = (uint32_t)abs8(llr[i]);
        s1 += a;
        s2 += a * a;
    }
    sums[0] = s1;
    sums[1] = s2;
}

int rx_parity(const int8_t *belief, const uint16_t *checks, unsigned count)
{
    int odd = 0;
    for (unsigned t = 0; t < 360; ++t) {
        int x = 0;
        for (unsigned j = 0; j < count; ++j) {
            x ^= belief[checks[2 * j] + (checks[2 * j + 1] + t - t % 16) % 360 + t % 16];
        }
        odd |= x < 0;
    }
    return odd;
}

void rx_hard(const int8_t *belief, const uint16_t *frames, unsigned groups, uint8_t *hard)
{
    for (unsigned g = 0; g < groups; ++g, belief += RX_SLOT, hard += 45) {
        for (unsigned i = 0; i < 46; ++i) {
            unsigned byte = 0;
            for (unsigned b = 0; b < 8; ++b) {
                const unsigned t = 8 * i + b;
                byte = (byte << 1) | (belief[(frames[g] + t - t % 16) % 360 + t % 16] < 0);
            }
            hard[i] = (uint8_t)byte;
        }
    }
}

/* ---------------------------------------------------------------- decoder */

/* One layer. Returns how many of its checks failed on the beliefs that pass 1 read:
 * zero over a whole iteration is a cheap sign of convergence, and the sum tracks progress. */
static HOT unsigned layer(rx_fec_t *f, unsigned L)
{
    const unsigned first = f->layer_first[L], count = f->layer_count[L];
    const unsigned repeats = f->layer_repeats[L], normal = count - repeats;
    const rx_edge_t *edges = f->edges + first;
    const uint16_t *writes = f->writes + first;
    int8_t *r = f->r + 16 * first * RX_VECTORS, *missing = NULL, saved = 0;
    if (L == 0) {
        missing = f->belief + f->missing_read;
        saved = *missing;
        *missing = 127;
        r[16 * f->missing_edge] = 0;
    }
    KERNEL(f, rx_pass1)(f->belief, f->reads + first * RX_VECTORS, r, f->qtmp, f->state, count);
    if (missing) {
        *missing = saved;
    }
    /* Failed checks per lane from pass 1, less the padding lanes 360..367. */
    unsigned odd = 0;
    const int8_t *failed = f->state + 64 * RX_VECTORS, *last = f->state + 64 * (RX_VECTORS - 1) + 48;
    for (unsigned lane = 0; lane < 16; ++lane) {
        odd += (unsigned)failed[lane] - (lane >= 8 && last[lane] < 0);
    }
    KERNEL(f, rx_pass2)(f->belief, writes, r, f->qtmp, f->state, normal | repeats << 8);
    if (missing) {
        /* Check 0's lane wrote p_(M-1) at position 0 of the new frame. */
        f->belief[writes[f->missing_edge]] = saved;
        r[16 * f->missing_edge] = 0;
    }
    KERNEL(f, rx_tails)(f->belief, writes, normal);
    for (unsigned j = normal; j < count; ++j) {
        KERNEL(f, rx_repeat)(f->belief + edges[j].home, f->belief + writes[j],
                             (edges[j].previous + 360 - edges[j].shift) % 360);
    }
    return odd;
}

/* Every check satisfied by the signs of the current beliefs. Layer 0 sees +127 for
 * the missing p_(-1); layer q - 1 reads the real p_(M-1) at the same position. */
static HOT int syndrome_ok(rx_fec_t *f)
{
    int8_t *missing = f->belief + f->missing_read, saved = *missing;
    *missing = 127;
    int odd = KERNEL(f, rx_parity)(f->belief, f->checks, f->layer_count[0]);
    *missing = saved;
    for (unsigned L = 1; L < f->q && !odd; ++L) {
        odd = KERNEL(f, rx_parity)(f->belief, f->checks + 2 * f->layer_first[L],
                                   f->layer_count[L]);
    }
    return !odd;
}

/* Channel LLRs into the slots, each in its starting frame. Then a gain in sixteenths
 * that aims at RX_UNITS integer units per natural LLR whatever the demodulator's scale
 * K: if x / K ~ N(+-1, s^2), the natural LLR is 2 x / (K s^2) = 2 m x / v with m and v
 * the mean and variance of |x| (|x| rarely folds over zero above about 1 dB Es/N0).
 * The gain is at most 4. */
static HOT void load(rx_fec_t *f, const int8_t *llr)
{
    const unsigned groups = f->groups, q = f->q;
    for (unsigned g = 0; g < RX_GROUPS; ++g) {
        int8_t *slot = f->belief + RX_SLOT * g;
        const unsigned frame = f->frame[g];
        if (g < groups) {
            memcpy(slot + frame, llr + 360 * g, 360 - frame);
            memcpy(slot, llr + 360 * g + 360 - frame, frame);
        } else {
            const int8_t *p = llr + f->nbch + (g - groups);
            for (unsigned t = 0, at = frame; t < 360; ++t, p += q, at = at == 359 ? 0 : at + 1) {
                slot[at] = *p;
            }
        }
        memcpy(slot + 360, slot, 16);
    }
    uint32_t sums[2];
    KERNEL(f, rx_moments)(llr, RX_STAT_VECTORS, sums);
    const uint64_t n = 16 * RX_STAT_VECTORS, s1 = sums[0], s2 = sums[1];
    const uint64_t den = n * s2 - s1 * s1;
    const uint64_t gain = den ? (32u * RX_UNITS * s1 * n + den / 2) / den : 64;
    KERNEL(f, rx_scale)(f->belief, RX_GROUPS, gain < 1 ? 1 : gain > 64 ? 64 : (unsigned)gain);
    memset(f->r, 0, 16 * f->edge_count * RX_VECTORS);
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
        if (!f->belief) {
            goto out;
        }
        load(f, frame->llr);
        unsigned before[2] = {~0u, ~0u};
        while (s.iterations < f->max_iterations) {
            unsigned odd = 0;
            ++s.iterations;
            for (unsigned L = 0; L < f->q; ++L) {
                odd += layer(f, L);
            }
            s.unsatisfied = (uint16_t)odd;
            const int stuck = s.iterations >= 3 && (odd >= before[1] || 6 * odd > 360 * f->q);
            before[1] = before[0];
            before[0] = odd;
            /* The layers saw satisfied checks, or this is the last chance. */
            if ((!odd || stuck || s.iterations == f->max_iterations) && syndrome_ok(f)) {
                KERNEL(f, rx_hard)(f->belief, f->frame, f->groups, frame->hard);
                s.ldpc_ok = 1;
                break;
            }
            if (stuck) {
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
