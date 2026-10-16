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
    } scratch __attribute__((aligned(16)));
    uint8_t carry[ROW] __attribute__((aligned(16)));
    uint8_t tail[16] __attribute__((aligned(16)));
    uint8_t header[90];
    uint8_t pilot_phases[22 * 36];
    uint8_t crc8[4][256];       /* CRC-8 of v followed by k zero bytes */
    uint16_t prog[MAX_PROG];
    uint16_t cols[360];         /* parity column t after the transpose: row offset in block 0 */
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

/* Aligned 32-bit access to byte buffers (Xtensa has no unaligned loads; a
 * memcpy here becomes a library call). */
typedef uint32_t __attribute__((may_alias, aligned(4))) word_t;
#define W(p) (*(word_t *)(p))

static inline uint32_t funnel(uint32_t lo, uint32_t hi, unsigned shift)
{
    return shift ? (lo >> shift) | (hi << (32 - shift)) : lo;
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
void dvbs2_pie_pack(const uint8_t *p, const uint16_t *cols, unsigned q, uint32_t *out,
                    uint32_t pending, unsigned have);
void dvbs2_pie_bswap(const uint32_t *src, uint8_t *block, unsigned vectors, unsigned sar_byte,
                     uint8_t *tail);
uint32_t dvbs2_pie_ldxq(const uint32_t *table);
uint32_t dvbs2_pie_crc8(const uint8_t *p, unsigned quads, const uint8_t *tables, uint32_t crc);
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
    /* After the transpose, rows 0..31 of column 32 w + c are big-endian in row
     * 8 (3 - c / 8) + c % 8, word w (see transpose32 and pack). */
    for (unsigned t = 0; t < 360; ++t) {
        const unsigned c = t % 32;
        f->cols[t] = (uint16_t)(ROW * (8 * (3 - c / 8) + c % 8) + 4 * (t / 32));
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
        f->crc8[0][v] = crc;
    }
    for (unsigned k = 1; k < 4; ++k) {
        for (unsigned v = 0; v < 256; ++v) {
            f->crc8[k][v] = f->crc8[0][f->crc8[k - 1][v]];
        }
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
    /* Aligned destination words from aligned source words, shifted together.
     * Start where the first source word lies inside data. */
    for (; k < 10 + n && (k < 14 || (k & 3)); ++k) {
        bits[k] = data[k - 10] ^ f->prbs[k];
    }
    if (k + 8 <= 10 + n) {
        const uintptr_t at = (uintptr_t)(data + k - 10);
        const unsigned shift = 8 * (at & 3);
        const uint8_t *src = (const uint8_t *)(at & ~(uintptr_t)3);
        uint32_t lo = W(src);
        for (; src + 8 <= data + n; k += 4) {
            src += 4;
            const uint32_t hi = W(src);
            W(bits + k) = funnel(lo, hi, shift) ^ W(f->prbs + k);
            lo = hi;
        }
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
    bb_header(ws->bits, f->crc8[0], 0x70, (unsigned)dfl_bits, 0, 0, 0);
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
    bb_header(ws->bits, f->crc8[0], 0xf0, (unsigned)(input_bytes * 8), 1504, 0x47, syncd);
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
        /* Four bytes per step: CRC-8 is linear, so
         * crc' = T3[crc ^ b0] ^ T2[b1] ^ T1[b2] ^ T0[b3], Tk = byte table^(k+1). */
        const uint8_t *t0 = f->crc8[0], *t1 = f->crc8[1], *t2 = f->crc8[2], *t3 = f->crc8[3];
#ifdef DVBS2_PIE
        if (f->pie & DVBS2_PIE_BB) {
            const unsigned quads = (unsigned)(end - p) / 4;
            crc = (uint8_t)dvbs2_pie_crc8(p, quads, t0, crc);
            p += 4 * quads;
        }
#endif
        for (; end - p >= 4; p += 4) {
            crc = t3[crc ^ p[0]] ^ t2[p[1]] ^ t1[p[2]] ^ t0[p[3]];
        }
        while (p != end) {
            crc = t0[crc ^ *p++];
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

#define BCH_ROWS(x)                                                                  \
    const uint8_t *r0 = t + (((x) & 255) << 4), *r1 = t + 8192 + (((x) >> 4) & 0xff0),   \
                  *r2 = t + 16384 + (((x) >> 12) & 0xff0), *r3 = t + 24576 + (((x) >> 20) & 0xff0)
#define BCH_WORD(j) (W(r0 + (j)) ^ W(r1 + (j)) ^ W(r2 + (j)) ^ W(r3 + (j)))

static HOT void bch_c(const dvbs2_fast *f, const uint8_t *bits, unsigned words, uint32_t r[8])
{
    const uint8_t *t = f->bch;
    const word_t *msg = (const word_t *)bits;
    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0, w5 = 0;
    if (f->bch_bytes > 16) {
        for (unsigned i = 0; i < words; ++i) {
            const uint32_t x = w0 ^ msg[i];
            BCH_ROWS(x);
            w0 = w1 ^ BCH_WORD(0);
            w1 = w2 ^ BCH_WORD(4);
            w2 = w3 ^ BCH_WORD(8);
            w3 = w4 ^ BCH_WORD(12);
            w4 = w5 ^ BCH_WORD(4096);
            w5 = BCH_WORD(4100);
        }
    } else {
        for (unsigned i = 0; i < words; ++i) {
            const uint32_t x = w0 ^ msg[i];
            BCH_ROWS(x);
            w0 = w1 ^ BCH_WORD(0);
            w1 = w2 ^ BCH_WORD(4);
            w2 = w3 ^ BCH_WORD(8);
            w3 = BCH_WORD(12);
        }
    }
    r[0] = w0;
    r[1] = w1;
    r[2] = w2;
    r[3] = w3;
    r[4] = w4;
    r[5] = w5;
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
            w[j] ^= W(lo + 4 * j);
        }
        w[4] ^= W(lo + 4096);
        w[5] ^= W(lo + 4100);
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
                const unsigned offset = *prog++, shift = 8 * (offset & 3);
                const uint8_t *window = f->scratch.ldpc.u2 + (offset & ~3u);
                uint32_t lo = W(window);
                for (unsigned w = 0; w < 12; ++w) {
                    const uint32_t hi = W(window + 4 * w + 4);
                    h[w] ^= funnel(lo, hi, shift);
                    lo = hi;
                }
            }
        }
        /* Accumulate down the rows: p_k ^= p_(k - q) in natural order. */
        for (unsigned w = 0; w < 12; ++w) {
            if (r) {
                h[w] ^= W(p + ROW * (r - 1) + 4 * w);
            }
            W(p + ROW * r + 4 * w) = h[w];
        }
    }
}

/* Hacker's Delight transpose32: afterwards a[k] bit i = old a[31 - i] bit 31 - k. */
static inline void transpose_stage(uint32_t a[32], unsigned j, uint32_t m)
{
    for (unsigned base = 0; base < 32; base += 2 * j) {
        for (unsigned k = base; k < base + j; ++k) {
            const uint32_t t = (a[k] ^ (a[k + j] >> j)) & m;
            a[k] ^= t;
            a[k + j] ^= t << j;
        }
    }
}

static inline void transpose32(uint32_t a[32])
{
    transpose_stage(a, 16, 0x0000ffffu);
    transpose_stage(a, 8, 0x00ff00ffu);
    transpose_stage(a, 4, 0x0f0f0f0fu);
    transpose_stage(a, 2, 0x33333333u);
    transpose_stage(a, 1, 0x55555555u);
}

static HOT void transpose_c(dvbs2_fast *f, unsigned blocks)
{
    uint8_t *p = f->scratch.ldpc.p;
    uint32_t a[32];
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned w = 0; w < 12; ++w) {
            const uint32_t carry = W(f->carry + 4 * w);
            for (unsigned i = 0; i < 32; ++i) {
                a[i] = W(p + ROW * (32 * b + i) + 4 * w) ^ carry;
            }
            transpose32(a);
            for (unsigned i = 0; i < 32; ++i) {
                W(p + ROW * (32 * b + i) + 4 * w) = a[i];
            }
        }
    }
}

static inline uint32_t bswap(uint32_t v)
{
    return __builtin_bswap32(v);
}

/* Low 32 bits of (hi:lo) >> s, s = 0..31: the Xtensa SRC instruction. */
static inline uint32_t src32(uint32_t hi, uint32_t lo, unsigned s)
{
    return (uint32_t)((((uint64_t)hi << 32) | lo) >> (s & 31));
}

/* Exclusive prefix parity along the last row: carry[t] = XOR of row bits
 * 0 .. t - 1 (the accumulator continues across columns). */
static HOT void carry_row(dvbs2_fast *f)
{
    const uint8_t *last = f->scratch.ldpc.p + ROW * (f->q - 1);
    uint32_t parity = 0;
    for (unsigned w = 0; w < 12; ++w) {
        uint32_t x = bswap(W(last + 4 * w));
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
        W(f->carry + 4 * w) = bswap(exclusive);
    }
}

/* Natural parity order: column t (q bits) follows column t - 1. After the
 * transpose, the 32 rows of block b for column 32 w + c sit big-endian in
 * row 32 b + 31 - sigma(c), word w, sigma(c) = 8 (c / 8) + 7 - c % 8.
 * Rows past q hold leftovers; only the top bits of each word are used. */
/* Natural parity order: column t (q bits) follows column t - 1. Output words
 * start at the word holding the parity start; its BCH bytes are kept by
 * starting the bit stream with them. Bits wait right-aligned in 'pending';
 * bits above 'have' are stale and never used. */
static unsigned pack_start(const uint8_t *bits, unsigned start, uint32_t *pending)
{
    *pending = 0;
    for (unsigned j = start - start % 4; j < start; ++j) {
        *pending = (*pending << 8) | bits[j];
    }
    return 8 * (start % 4);
}

static HOT void pack(dvbs2_fast *f, uint8_t *bits)
{
    const unsigned q = f->q, start = f->mode->nbch / 8, full = q / 32, part = q % 32;
    const uint8_t *p = f->scratch.ldpc.p;
    word_t *out = (word_t *)(bits + start - start % 4);
    uint32_t pending;
    unsigned have = pack_start(bits, start, &pending);
    for (unsigned t = 0; t < 360; ++t) {
        const uint8_t *col = p + f->cols[t];
        for (unsigned b = 0; b < full; ++b) {
            const uint32_t v = W(col + ROW * 32 * b);
            *out++ = bswap(src32(pending, v, have));
            pending = v;
        }
        const uint32_t v = W(col + ROW * 32 * full);
        if (have + part >= 32) {
            *out++ = bswap(src32(pending, v, have));
            pending = v >> (32 - part);
            have += part - 32;
        } else {
            pending = src32(pending, v, 32 - part);
            have += part;
        }
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
        /* Pack big-endian words into the free u2 area, then reverse their
         * bytes into ws->bits from the word holding the parity start. */
        const unsigned start = f->mode->nbch / 8, base = start - start % 4;
        const unsigned words = (DVBS2_FEC_BYTES - base) / 4, vectors = words / 4;
        uint32_t pending;
        const unsigned have = pack_start(ws->bits, start, &pending);
        uint32_t *packed = (uint32_t *)f->scratch.ldpc.u2;
        dvbs2_pie_pack(f->scratch.ldpc.p, f->cols, f->q, packed, pending, have);
        uint8_t *dst = ws->bits + base;
        const uintptr_t at = (uintptr_t)dst;
        uint8_t *block = (uint8_t *)((at - 1) & ~(uintptr_t)15);
        uint8_t keep[16];   /* BCH bytes between block and dst */
        memcpy(keep, block, (size_t)(dst - block));
        dvbs2_pie_bswap(packed, block, vectors, (unsigned)(-at & 15), f->tail);
        memcpy(block, keep, (size_t)(dst - block));
        memcpy(block + 16 * vectors, f->tail, (at & 15) ? (at & 15) : 16);
        for (unsigned i = 4 * vectors; i < words; ++i) {
            W(dst + 4 * i) = bswap(packed[i]);
        }
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

/* Writes a run of 4-byte words to any address with aligned stores, like the
 * PIE kernel: the word before the run and the bytes after it are scribbled
 * on (header or pilot positions, rewritten by dvbs2_fast_pl), except at the
 * frame end where the last bytes go one by one. */
typedef struct {
    word_t *at;
    uint32_t previous;
    unsigned shift;
} writer;

static inline writer writer_start(uint8_t *dst)
{
    const uintptr_t a = (uintptr_t)dst;
    writer w = {(word_t *)(a & ~(uintptr_t)3), 0, 8 * (unsigned)(a & 3)};
    return w;
}

static inline void writer_put(writer *w, uint32_t v)
{
    if (w->shift) {
        *w->at++ = (w->previous >> (32 - w->shift)) | (v << w->shift);
        w->previous = v;
    } else {
        *w->at++ = v;
    }
}

static inline void writer_end(writer *w, int last)
{
    if (!w->shift) {
        return;
    }
    uint8_t *tail = (uint8_t *)w->at;
    if (last) {
        for (unsigned i = 0; i < w->shift / 8; ++i) {
            tail[i] = (uint8_t)(w->previous >> (32 - w->shift + 8 * i));
        }
    } else {
        *w->at = w->previous >> (32 - w->shift);
    }
}

/* Data symbol k goes to output 90 + k + 36 (k / 1440) with pilots. Runs of
 * 1440 symbols are whole groups of four, so a table word never straddles a
 * pilot block. */
static HOT void map_c(const dvbs2_fast *f, const uint8_t *bits, uint8_t *output)
{
    const unsigned d = f->data_symbols, runs = f->pilots ? f->pilot_blocks + 1 : 1;
    const unsigned run_symbols = f->pilots ? 1440 : d;
    const uint32_t *map = f->map;
    const uint8_t *a = bits, *b = bits + 2700, *c = bits + 5400;
    if (f->mode->modcod == 12) {   /* 5.3.3: rate 3/5 reads the columns backwards */
        a = bits + 5400;
        c = bits;
    }
    for (unsigned r = 0; r < runs; ++r) {
        const unsigned k0 = r * run_symbols;
        const unsigned n = d - k0 < run_symbols ? d - k0 : run_symbols;
        writer w = writer_start(output + 90 + k0 + 36 * r);
        if (f->mode->bits_per_symbol == 3) {
            for (unsigned j = k0 / 8; j < (k0 + n) / 8; ++j) {
                const unsigned x = b[j] ^ c[j], z0 = f->gold[j], z1 = f->gold[GOLD + j];
                const unsigned p0 = a[j] ^ x;                       /* inverted p0 */
                const unsigned p1 = a[j] ^ z0;
                const unsigned p2 = b[j] ^ (a[j] & x) ^ z1 ^ (a[j] & z0);
                writer_put(&w, map[(p0 >> 4) | (p2 & 0xf0) | (p1 >> 4) << 8]);
                writer_put(&w, map[(p0 & 15) | (p2 & 15) << 4 | (p1 & 15) << 8]);
            }
        } else {
            for (unsigned j = k0 / 4; j < (k0 + n) / 4; ++j) {
                const unsigned x = bits[j], g = f->gold[j];
                const unsigned dd = x ^ (x >> 1);
                const unsigned p2 = x ^ (g >> 1) ^ (dd & g), p1 = dd ^ g;
                writer_put(&w, map[(p1 & 0x55) | (p2 & 0x55) << 1]);
            }
        }
        writer_end(&w, r == runs - 1);
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
