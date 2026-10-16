/* Fast normal-frame encoder: the stages of dvbs2.c reorganized for word and
 * 128-bit operations. Every stage keeps the canonical workspace format
 * (ws->bits, MSB first), so each one can be checked and timed on its own.
 *
 *   BB      payload copy, header and scrambling in one pass; TS CRC-8 by table.
 *   BCH     32 message bits per step, four 256-row tables (one per byte).
 *   LDPC    whole 360-bit rows: each parity row r of the q x 360 group form is
 *           a sum of byte-aligned windows of the doubled information group,
 *           shifted by 0..7 bits with a Horner scheme; then a 32x32 bit
 *           transpose and a bit packer give the natural parity order.
 *   MAP     interleaver, mapping and PL scrambling as bit-plane logic, four
 *           symbols per table lookup, then pilots and header.
 *
 * The C here is the portable reference of the same data layout. On the
 * ESP32-S3 (DVBS2_PIE) the hot loops can run as PIE kernels from pie.S. */
#include "dvbs2.h"
#include "private.h"

#include <string.h>

#ifdef DVBS2_IRAM
#define HOT __attribute__((section(".iram1"), noinline))
#else
#define HOT
#endif

#define MAX_GROUPS 162          /* 9/10: 58320 / 360 */
#define MAX_Q 90                /* 1/2 */
#define P_ROWS 96               /* q rounded up to whole 32-row transpose blocks */
#define ROW 48                  /* bytes per parity row: 360 bits + 24 spare */
#define U2 96                   /* bytes per doubled group: 744 bits needed */
#define MAX_PROG (MAX_Q * 9 + 648)    /* rows x (1 + 8 levels) + edges */
#define GOLD 2720               /* one 8PSK Gold plane, 169 x 16 bytes rounded */

struct dvbs2_fast {
    /* BCH: slice k (byte k of a 32-bit message word, k = 0 first) at
     * 8192 k; value v at +16 v; remainder bytes 0..15 there, 16..23 at +4096. */
    uint8_t bch[4 * 8192] __attribute__((aligned(16)));
    /* Four phase bytes per 12-bit (8PSK) or 8-bit (QPSK) index. */
    uint32_t map[4096] __attribute__((aligned(16)));
    /* PL scrambling as bit planes in data-symbol order, MSB first:
     * 8PSK: Z0 = R & 1 at 0, Z1 = R >> 1 at GOLD.
     * QPSK: one byte per four symbols, Z1 at bit 7 - 2i, Z0 at bit 6 - 2i. */
    uint8_t gold[8128] __attribute__((aligned(16)));
    uint8_t prbs[7296] __attribute__((aligned(16)));
    union {
        struct {
            uint8_t u2[MAX_GROUPS * U2];
            uint8_t p[P_ROWS * ROW];
        } ldpc;
        uint16_t index[507 * 16];   /* QPSK: 507 chunks of 64 symbols */
        uint32_t packed[675 + 8];   /* parity in big-endian words */
    } scratch __attribute__((aligned(16)));
    uint8_t carry[ROW] __attribute__((aligned(16)));
    uint8_t tail[16] __attribute__((aligned(16)));
    uint8_t header[90];
    uint8_t pilot_phases[22 * 36];
    uint8_t crc8[256];
    uint16_t prog[MAX_PROG];
    const dvbs2_mode *mode;
    unsigned pilots, pie, ldxq_bias;
    unsigned bch_bytes, groups, q, data_symbols, pilot_blocks;
};

size_t dvbs2_fast_size(void)
{
    return sizeof(dvbs2_fast);
}

unsigned dvbs2_fast_pie(const dvbs2_fast *f)
{
    return f->pie;
}

int dvbs2_fast_ldxq_bias(const dvbs2_fast *f)
{
    return (f->pie & DVBS2_PIE_MAP) ? (int)f->ldxq_bias : -1;
}

static inline uint32_t load32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void store32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

#ifdef DVBS2_PIE
/* pie.S */
void dvbs2_pie_xor(uint8_t *dst, const uint8_t *src, const uint8_t *prbs, unsigned blocks);
void dvbs2_pie_bch16(const uint8_t *msg, unsigned words, const uint8_t *tables, uint8_t *r);
void dvbs2_pie_bch32(const uint8_t *msg, unsigned words, const uint8_t *tables, uint8_t *r);
void dvbs2_pie_u2(const uint8_t *bits, uint8_t *u2, unsigned groups);
void dvbs2_pie_rows(const uint8_t *u2, const uint16_t *prog, uint8_t *p, unsigned q);
void dvbs2_pie_transpose(uint8_t *p, unsigned blocks, const uint8_t *carry);
void dvbs2_pie_index8(const uint8_t *a, const uint8_t *b, const uint8_t *c,
                      const uint8_t *gold, uint16_t *index, unsigned chunks);
void dvbs2_pie_index4(const uint8_t *x, const uint8_t *gold, uint16_t *index, unsigned chunks);
void dvbs2_pie_run(const uint16_t *index, const uint32_t *table, uint8_t *block,
                   unsigned vectors, unsigned sar_byte, uint8_t *tail);
uint32_t dvbs2_pie_ldxq(const uint32_t *table);
#define PIE_MASK (DVBS2_PIE_BB | DVBS2_PIE_BCH | DVBS2_PIE_LDPC | DVBS2_PIE_MAP)
#else
#define PIE_MASK 0u
#endif

/* ---------------------------------------------------------------- prepare */

static void prepare_bch(dvbs2_fast *f)
{
    const uint32_t (*bytes)[6] = dvbs2_bch_bytes(f->mode->t);
    const unsigned n = f->bch_bytes;
    uint8_t row[24];
    memset(f->bch, 0, sizeof f->bch);
    for (unsigned v = 0; v < 256; ++v) {
        /* Slice 3 is the byte table: v(x) x^r mod g(x), as bytes MSB first. */
        for (unsigned j = 0; j < n; ++j) {
            row[j] = (uint8_t)(bytes[v][j / 4] >> (24 - 8 * (j % 4)));
        }
        for (int k = 3; k >= 0; --k) {
            uint8_t *lo = f->bch + 8192 * k + 16 * v;
            memcpy(lo, row, 16);
            memcpy(lo + 4096, row + 16, n - 16);
            /* Next slice: one more byte of x^8, reduced with the byte table. */
            const unsigned top = row[0];
            memmove(row, row + 1, n - 1);
            row[n - 1] = 0;
            for (unsigned j = 0; j < n; ++j) {
                row[j] ^= (uint8_t)(bytes[top][j / 4] >> (24 - 8 * (j % 4)));
            }
        }
    }
}

/* Row r of the group form gets, for each table address x = r + q c of group g,
 * the information group rotated by c: P[r][t] ^= u[(t - c) mod 360]. With the
 * doubled group u2 (u2[i] = u[i mod 360]) that is u2[t + s], s = (360 - c) mod
 * 360: a window starting at byte s / 8, read s % 8 bits further on. Rows are
 * built as H = sum over shifts b = bmax..0 of shift_b(sum of windows), Horner
 * style: H = shift_1(H) ^ windows(b). Program per row: bmax + 1, then for each
 * b from bmax down to 0 the window count and the u2 byte offsets. */
static int prepare_ldpc(dvbs2_fast *f)
{
    const uint16_t *table = dvbs2_ldpc_table(f->mode->modcod);
    uint16_t *out = f->prog;
    for (unsigned r = 0; r < f->q; ++r) {
        uint16_t offset[64];
        uint8_t shift[64];
        unsigned n = 0, bmax = 0;
        const uint16_t *row = table;
        for (unsigned g = 0; g < f->groups; ++g) {
            const unsigned degree = *row++;
            for (unsigned e = 0; e < degree; ++e) {
                const unsigned x = *row++, s = (360 - (x & 511)) % 360;
                if (x >> 9 != r) {
                    continue;
                }
                if (n == 64) {
                    return -1;
                }
                offset[n] = (uint16_t)(U2 * g + s / 8);
                shift[n] = (uint8_t)(s & 7);
                bmax = shift[n] > bmax ? shift[n] : bmax;
                ++n;
            }
        }
        if (out + 1 + (bmax + 1) + n > f->prog + MAX_PROG) {
            return -1;
        }
        *out++ = (uint16_t)(bmax + 1);
        for (int b = (int)bmax; b >= 0; --b) {
            uint16_t *count = out++;
            *count = 0;
            for (unsigned e = 0; e < n; ++e) {
                if (shift[e] == b) {
                    *out++ = offset[e];
                    ++*count;
                }
            }
        }
    }
    memset(&f->scratch, 0, sizeof f->scratch);
    return 0;
}

static unsigned data_position(const dvbs2_fast *f, unsigned k)
{
    return k + (f->pilots ? 36 * (k / 1440) : 0);
}

static void prepare_map(dvbs2_fast *f)
{
    const unsigned d = f->data_symbols;
    memset(f->gold, 0, sizeof f->gold);
    if (f->mode->bits_per_symbol == 3) {
        /* Index bits 3..0, 7..4, 11..8: inverted p0, p2, p1 of symbols 0..3,
         * first symbol in the highest bit of each nibble. */
        for (unsigned v = 0; v < 4096; ++v) {
            uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const unsigned p0 = ((v >> (3 - i)) & 1) ^ 1, p2 = (v >> (7 - i)) & 1,
                               p1 = (v >> (11 - i)) & 1;
                word |= (uint32_t)(p0 | p1 << 1 | p2 << 2) << (8 * i);
            }
            f->map[v] = word;
        }
        for (unsigned k = 0; k < d; ++k) {
            const unsigned r = dvbs2_pl_rotation(data_position(f, k));
            f->gold[k / 8] |= (uint8_t)((r & 1) << (7 - k % 8));
            f->gold[GOLD + k / 8] |= (uint8_t)((r >> 1) << (7 - k % 8));
        }
    } else {
        /* Index bit 6 - 2i holds p1, bit 7 - 2i p2 of symbol i; p0 = 1. */
        for (unsigned v = 0; v < 256; ++v) {
            uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const unsigned p1 = (v >> (6 - 2 * i)) & 1, p2 = (v >> (7 - 2 * i)) & 1;
                word |= (uint32_t)(1 | p1 << 1 | p2 << 2) << (8 * i);
            }
            f->map[v] = word;
        }
        for (unsigned k = 0; k < d; ++k) {
            const unsigned r = dvbs2_pl_rotation(data_position(f, k)), i = k % 4;
            f->gold[k / 4] |= (uint8_t)(((r >> 1) << (7 - 2 * i)) | ((r & 1) << (6 - 2 * i)));
        }
    }
    dvbs2_plheader(f->mode->modcod, (int)f->pilots, f->header);
    for (unsigned b = 0; b < f->pilot_blocks; ++b) {
        for (unsigned i = 0; i < 36; ++i) {
            const unsigned p = 1440 * (b + 1) + 36 * b + i;
            f->pilot_phases[36 * b + i] = (uint8_t)((1 + 2 * dvbs2_pl_rotation(p)) & 7);
        }
    }
}

int dvbs2_fast_prepare(dvbs2_fast *f, const dvbs2_mode *mode, int pilots, unsigned pie)
{
    if (!f || !mode || dvbs2_find_mode(mode->modcod) != mode) {
        return -1;
    }
    f->mode = mode;
    f->pilots = pilots ? 1 : 0;
    f->bch_bytes = (mode->nbch - mode->kbch) / 8;
    f->groups = mode->nbch / 360;
    f->q = mode->q;
    f->data_symbols = 64800 / mode->bits_per_symbol;
    f->pilot_blocks = pilots ? (f->data_symbols / 90 - 1) / 16 : 0;
    f->pie = pie & PIE_MASK;
    f->ldxq_bias = 0;
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_MAP) {
        /* The TRM gives EE.LDXQ.32 the address as + 4 index; Espressif's QEMU
         * uses as + 4 index - 4. Measure it once and compensate. */
        static const uint32_t probe[4] __attribute__((aligned(16))) = {
            0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        const uint32_t seen = dvbs2_pie_ldxq(probe + 1);
        if (seen == probe[2]) {
            f->ldxq_bias = 0;
        } else if (seen == probe[1]) {
            f->ldxq_bias = 4;
        } else {
            f->pie &= ~(unsigned)DVBS2_PIE_MAP;
        }
    }
#endif
    memcpy(f->prbs, dvbs2_prbs_table(), mode->kbch / 8);
    for (unsigned v = 0; v < 256; ++v) {
        uint8_t crc = (uint8_t)v;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0xd5 : 0));
        }
        f->crc8[v] = crc;
    }
    prepare_bch(f);
    if (prepare_ldpc(f) != 0) {
        return -1;
    }
    prepare_map(f);
    return 0;
}

/* ---------------------------------------------------------------- BB */

static void bb_header(uint8_t *out, const uint8_t *crc8, uint8_t matype, unsigned dfl,
                      unsigned upl, uint8_t sync, unsigned syncd)
{
    out[0] = matype;
    out[1] = 0;
    out[2] = (uint8_t)(upl >> 8);
    out[3] = (uint8_t)upl;
    out[4] = (uint8_t)(dfl >> 8);
    out[5] = (uint8_t)dfl;
    out[6] = sync;
    out[7] = (uint8_t)(syncd >> 8);
    out[8] = (uint8_t)syncd;
    uint8_t crc = 0;
    for (unsigned k = 0; k < 9; ++k) {
        crc = crc8[crc ^ out[k]];
    }
    out[9] = crc;
}

/* bits[10 .. 10 + n) = data ^ prbs, the rest of the frame = prbs. */
static HOT void bb_fill(const dvbs2_fast *f, uint8_t *bits, const uint8_t *data, size_t n)
{
    const size_t frame = f->mode->kbch / 8;
    size_t k = 10;
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_BB) {
        /* Whole 16-byte blocks from byte 16 whose 16-byte source over-read
         * stays inside data[0 .. n). */
        const size_t first = 16, avail = n + 10 > 32 ? n + 10 - 32 : 0;
        for (; k < first; ++k) {
            bits[k] = (k - 10 < n ? data[k - 10] : 0) ^ f->prbs[k];
        }
        const unsigned blocks = (unsigned)(avail / 16);
        if (blocks) {
            dvbs2_pie_xor(bits + 16, data + 6, f->prbs + 16, blocks);
            k = 16 + 16 * (size_t)blocks;
        }
    }
#endif
    for (; k + 4 <= 10 + n; k += 4) {
        store32(bits + k, load32(data + k - 10) ^ load32(f->prbs + k));
    }
    for (; k < 10 + n; ++k) {
        bits[k] = data[k - 10] ^ f->prbs[k];
    }
    for (; k < frame; ++k) {
        bits[k] = f->prbs[k];
    }
    for (k = 0; k < 10; ++k) {
        bits[k] ^= f->prbs[k];
    }
}

HOT int dvbs2_fast_bb_gcs(const dvbs2_fast *f, dvbs2_workspace *ws,
                          const uint8_t *payload, size_t dfl_bits)
{
    if (!f || !ws || dfl_bits > (size_t)f->mode->kbch - 80 || (!payload && dfl_bits)) {
        return -1;
    }
    const size_t n = (dfl_bits + 7) / 8;
    bb_header(ws->bits, f->crc8, 0x70, (unsigned)dfl_bits, 0, 0, 0);
    bb_fill(f, ws->bits, payload, n);
    if (dfl_bits & 7) {
        /* Clear the unused low bits of the last data byte, then rescramble. */
        const size_t last = 10 + dfl_bits / 8;
        const uint8_t keep = (uint8_t)(0xff << (8 - (dfl_bits & 7)));
        ws->bits[last] = (uint8_t)(((ws->bits[last] ^ f->prbs[last]) & keep) ^ f->prbs[last]);
    }
    return 0;
}

HOT int dvbs2_fast_bb_ts(const dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *state,
                         const uint8_t *input, size_t input_bytes)
{
    if (!f || !ws || !state || !input || state->packet_pos >= 188 ||
        input_bytes != ((size_t)f->mode->kbch - 80) / 8) {
        return -1;
    }
    const size_t first_sync = state->packet_pos ? 188 - state->packet_pos : 0;
    for (size_t k = first_sync; k < input_bytes; k += 188) {
        if (input[k] != 0x47) {
            return -1;
        }
    }
    const unsigned syncd = ((188 - state->packet_pos) % 188) * 8;
    bb_header(ws->bits, f->crc8, 0xf0, (unsigned)(input_bytes * 8), 1504, 0x47, syncd);
    bb_fill(f, ws->bits, input, input_bytes);
    /* CRC-8 of each packet's 187 bytes after sync; the sync byte position
     * carries the previous packet's CRC. */
    uint8_t crc = state->crc, previous = state->previous_crc;
    size_t k = 0;
    while (k < input_bytes) {
        if (state->packet_pos == 0) {
            ws->bits[10 + k] = previous ^ f->prbs[10 + k];
            crc = 0;
            ++k;
            state->packet_pos = 1;
            continue;
        }
        size_t run = 188 - state->packet_pos;
        if (run > input_bytes - k) {
            run = input_bytes - k;
        }
        const uint8_t *p = input + k, *end = p + run;
        while (p != end) {
            crc = f->crc8[crc ^ *p++];
        }
        k += run;
        state->packet_pos = (uint16_t)((state->packet_pos + run) % 188);
        if (state->packet_pos == 0) {
            previous = crc;
        }
    }
    state->crc = crc;
    state->previous_crc = previous;
    return 0;
}

/* ---------------------------------------------------------------- BCH */

static HOT void bch_c(const dvbs2_fast *f, uint8_t *bits, unsigned words, uint32_t w[8])
{
    const unsigned hi = f->bch_bytes > 16;
    for (unsigned i = 0; i < words; ++i) {
        const uint32_t x = w[0] ^ load32(bits + 4 * i);
        w[0] = w[1];
        w[1] = w[2];
        w[2] = w[3];
        w[3] = w[4];
        w[4] = w[5];
        w[5] = 0;
        for (unsigned k = 0; k < 4; ++k) {
            const uint8_t *lo = f->bch + 8192 * k + 16 * ((x >> (8 * k)) & 255);
            w[0] ^= load32(lo);
            w[1] ^= load32(lo + 4);
            w[2] ^= load32(lo + 8);
            w[3] ^= load32(lo + 12);
            if (hi) {
                w[4] ^= load32(lo + 4096);
                w[5] ^= load32(lo + 4100);
            }
        }
    }
}

HOT void dvbs2_fast_bch(dvbs2_fast *f, dvbs2_workspace *ws)
{
    const unsigned bytes = f->mode->kbch / 8, words = bytes / 4;
    uint32_t w[8] __attribute__((aligned(16))) = {0};
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_BCH) {
        (f->bch_bytes > 16 ? dvbs2_pie_bch32 : dvbs2_pie_bch16)(ws->bits, words, f->bch,
                                                               (uint8_t *)w);
    } else
#endif
    {
        bch_c(f, ws->bits, words, w);
    }
    /* Remaining 0..3 message bytes, one byte-table step each. */
    for (unsigned i = 4 * words; i < bytes; ++i) {
        const unsigned v = (w[0] & 255) ^ ws->bits[i];
        for (unsigned j = 0; j < 5; ++j) {
            w[j] = (w[j] >> 8) | (w[j + 1] << 24);
        }
        w[5] >>= 8;
        const uint8_t *lo = f->bch + 8192 * 3 + 16 * v;
        for (unsigned j = 0; j < 4; ++j) {
            w[j] ^= load32(lo + 4 * j);
        }
        w[4] ^= load32(lo + 4096);
        w[5] ^= load32(lo + 4100);
    }
    memcpy(ws->bits + bytes, w, f->bch_bytes);
}

/* ---------------------------------------------------------------- LDPC */

/* Read one bit further along the MSB-first stream: byte j becomes
 * (b[j] << 1) | (b[j + 1] >> 7). */
static inline void shift_1(uint32_t h[12])
{
    for (unsigned w = 0; w < 12; ++w) {
        const uint32_t next = w < 11 ? h[w + 1] : 0;
        h[w] = ((h[w] << 1) & 0xfefefefeu) | ((h[w] >> 15) & 0x00010101u) |
               ((next << 17) & 0x01000000u);
    }
}

static HOT void rows_c(dvbs2_fast *f)
{
    const uint16_t *prog = f->prog;
    uint8_t *p = f->scratch.ldpc.p;
    for (unsigned r = 0; r < f->q; ++r) {
        uint32_t h[12] = {0};
        const unsigned levels = *prog++;
        for (unsigned level = 0; level < levels; ++level) {
            if (level) {
                shift_1(h);
            }
            for (unsigned n = *prog++; n; --n) {
                const uint8_t *window = f->scratch.ldpc.u2 + *prog++;
                for (unsigned w = 0; w < 12; ++w) {
                    h[w] ^= load32(window + 4 * w);
                }
            }
        }
        /* Accumulate down the rows: p_k ^= p_(k - q) in natural order. */
        for (unsigned w = 0; w < 12; ++w) {
            if (r) {
                h[w] ^= load32(p + ROW * (r - 1) + 4 * w);
            }
            store32(p + ROW * r + 4 * w, h[w]);
        }
    }
}

/* Hacker's Delight transpose32: afterwards a[k] bit i = old a[31 - i] bit 31 - k. */
static inline void transpose32(uint32_t a[32])
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

static HOT void transpose_c(dvbs2_fast *f, unsigned blocks)
{
    uint8_t *p = f->scratch.ldpc.p;
    uint32_t a[32];
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned w = 0; w < 12; ++w) {
            const uint32_t carry = load32(f->carry + 4 * w);
            for (unsigned i = 0; i < 32; ++i) {
                a[i] = load32(p + ROW * (32 * b + i) + 4 * w) ^ carry;
            }
            transpose32(a);
            for (unsigned i = 0; i < 32; ++i) {
                store32(p + ROW * (32 * b + i) + 4 * w, a[i]);
            }
        }
    }
}

static inline uint32_t bswap(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

/* Exclusive prefix parity along the last row: carry[t] = XOR of row bits
 * 0 .. t - 1 (the accumulator continues across columns). */
static HOT void carry_row(dvbs2_fast *f)
{
    const uint8_t *last = f->scratch.ldpc.p + ROW * (f->q - 1);
    uint32_t parity = 0;
    for (unsigned w = 0; w < 12; ++w) {
        uint32_t x = bswap(load32(last + 4 * w));
        if (w == 11) {
            x &= 0xff000000u;   /* bits 352..359 only */
        }
        x ^= x >> 1;
        x ^= x >> 2;
        x ^= x >> 4;
        x ^= x >> 8;
        x ^= x >> 16;
        const uint32_t exclusive = (x >> 1) ^ (parity ? 0xffffffffu : 0);
        parity ^= x & 1;
        store32(f->carry + 4 * w, bswap(exclusive));
    }
}

/* Natural parity order: column t (q bits) follows column t - 1. After the
 * transpose, the 32 rows of block b for column 32 w + c sit big-endian in
 * row 32 b + 31 - sigma(c), word w, sigma(c) = 8 (c / 8) + 7 - c % 8.
 * Rows past q hold leftovers; only the top bits of each word are used. */
static HOT void pack(dvbs2_fast *f, uint8_t *bits)
{
    const unsigned q = f->q, start = f->mode->nbch / 8;
    const uint8_t *p = f->scratch.ldpc.p;
    uint32_t *out = f->scratch.packed;
    /* Begin at the word holding the parity start; keep its BCH bytes. */
    const unsigned lead = start % 4;
    uint32_t pending = 0;
    unsigned have = 8 * lead;
    for (unsigned j = 0; j < lead; ++j) {
        pending = (pending << 8) | bits[start - lead + j];
    }
    unsigned n = 0;
    for (unsigned t = 0; t < 360; ++t) {
        const unsigned w = t / 32, c = t % 32;
        const unsigned slot = 8 * (3 - c / 8) + c % 8;
        for (unsigned b = 0; 32 * b < q; ++b) {
            const uint32_t v = load32(p + ROW * (32 * b + slot) + 4 * w);
            const unsigned length = q - 32 * b < 32 ? q - 32 * b : 32;
            /* Append the top 'length' bits of v to the pending bits. */
            if (have + length >= 32) {
                out[n++] = have ? (pending << (32 - have)) | (v >> have) : v;
                const unsigned left = have + length - 32;
                pending = left ? v >> (32 - length) : 0;
                have = left;
            } else {
                pending = (pending << length) | (v >> (32 - length));
                have += length;
            }
        }
    }
    uint8_t *dst = bits + start - lead;
    for (unsigned i = 0; i < n; ++i) {
        store32(dst + 4 * i, bswap(out[i]));
    }
}

HOT void dvbs2_fast_ldpc(dvbs2_fast *f, dvbs2_workspace *ws)
{
    const unsigned blocks = (f->q + 31) / 32;
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_LDPC) {
        dvbs2_pie_u2(ws->bits, f->scratch.ldpc.u2, f->groups);
        dvbs2_pie_rows(f->scratch.ldpc.u2, f->prog, f->scratch.ldpc.p, f->q);
        carry_row(f);
        dvbs2_pie_transpose(f->scratch.ldpc.p, blocks, f->carry);
        pack(f, ws->bits);
        return;
    }
#endif
    for (unsigned g = 0; g < f->groups; ++g) {
        uint8_t *u2 = f->scratch.ldpc.u2 + U2 * g;
        const uint8_t *u = ws->bits + 45 * g;
        memcpy(u2, u, 45);
        memcpy(u2 + 45, u, 45);
        memcpy(u2 + 90, u, 6);
    }
    rows_c(f);
    carry_row(f);
    transpose_c(f, blocks);
    pack(f, ws->bits);
}

/* ---------------------------------------------------------------- mapping */

/* Data symbol k goes to output 90 + k + 36 (k / 1440) with pilots. Runs of
 * 1440 symbols are whole groups of four, so a table word never straddles a
 * pilot block. */
static HOT void map_c(const dvbs2_fast *f, const uint8_t *bits, uint8_t *output)
{
    const unsigned d = f->data_symbols;
    uint8_t *run = output + 90;
    if (f->mode->bits_per_symbol == 3) {
        const uint8_t *a = bits, *b = bits + 2700, *c = bits + 5400;
        if (f->mode->modcod == 12) {   /* 5.3.3: rate 3/5 reads the columns backwards */
            a = bits + 5400;
            c = bits;
        }
        for (unsigned j = 0; j < d / 8; ++j) {
            const unsigned x = b[j] ^ c[j], z0 = f->gold[j], z1 = f->gold[GOLD + j];
            const unsigned p0 = a[j] ^ x;                       /* inverted p0 */
            const unsigned p1 = a[j] ^ z0;
            const unsigned p2 = b[j] ^ (a[j] & x) ^ z1 ^ (a[j] & z0);
            const unsigned k = 8 * j;
            uint8_t *o = run + k + (f->pilots ? 36 * (k / 1440) : 0);
            store32(o, f->map[(p0 >> 4) | (p2 & 0xf0) | (p1 >> 4) << 8]);
            store32(o + 4, f->map[(p0 & 15) | (p2 & 15) << 4 | (p1 & 15) << 8]);
        }
    } else {
        for (unsigned j = 0; j < d / 4; ++j) {
            const unsigned x = bits[j], g = f->gold[j];
            const unsigned dd = x ^ (x >> 1);
            const unsigned p2 = x ^ (g >> 1) ^ (dd & g), p1 = dd ^ g;
            const unsigned k = 4 * j;
            store32(run + k + (f->pilots ? 36 * (k / 1440) : 0),
                    f->map[(p1 & 0x55) | (p2 & 0x55) << 1]);
        }
    }
}

HOT void dvbs2_fast_pl(const dvbs2_fast *f, uint8_t *output)
{
    memcpy(output, f->header, 90);
    for (unsigned b = 0; b < f->pilot_blocks; ++b) {
        memcpy(output + 90 + 1440 * (b + 1) + 36 * b, f->pilot_phases + 36 * b, 36);
    }
}

#ifdef DVBS2_PIE
/* Index pass over the whole frame, then one shifted-store pass per run. */
static HOT void map_pie(dvbs2_fast *f, const uint8_t *bits, uint8_t *output)
{
    const unsigned d = f->data_symbols;
    uint16_t *index = f->scratch.index;
    if (f->mode->bits_per_symbol == 3) {
        const uint8_t *a = bits, *c = bits + 5400;
        if (f->mode->modcod == 12) {
            a = bits + 5400;
            c = bits;
        }
        dvbs2_pie_index8(a, bits + 2700, c, f->gold, index, (d + 127) / 128);
    } else {
        dvbs2_pie_index4(bits, f->gold, index, (d + 63) / 64);
    }
    const uint32_t *table = (const uint32_t *)((const uint8_t *)f->map + f->ldxq_bias);
    const unsigned runs = f->pilots ? f->pilot_blocks + 1 : 1;
    for (unsigned r = 0; r < runs; ++r) {
        const unsigned k = r * 1440;
        const unsigned length = f->pilots ? (d - k < 1440 ? d - k : 1440) : d;
        uint8_t *dst = output + 90 + k + 36 * r;
        const uintptr_t at = (uintptr_t)dst;
        uint8_t *block = (uint8_t *)((at - 1) & ~(uintptr_t)15);
        const unsigned vectors = length / 16;
        dvbs2_pie_run(index + k / 4, table, block, vectors, (unsigned)(-at & 15), f->tail);
        /* The kernel leaves the final aligned block in f->tail: the run's
         * last (at & 15) bytes, or a whole vector when dst is aligned. */
        memcpy(block + 16 * vectors, f->tail, (at & 15) ? (at & 15) : 16);
    }
}
#endif

HOT void dvbs2_fast_map(dvbs2_fast *f, const dvbs2_workspace *ws, uint8_t *output)
{
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_MAP) {
        map_pie(f, ws->bits, output);
    } else
#endif
    {
        map_c(f, ws->bits, output);
    }
}
