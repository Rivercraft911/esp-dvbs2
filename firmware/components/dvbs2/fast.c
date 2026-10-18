/* Fast normal-frame encoder: the stages of dvbs2.c reorganized for word and
 * 128-bit operations. Every stage keeps the canonical workspace format
 * (ws->bits, MSB first), so each one can be checked and timed on its own.
 *
 *   BB      payload copy, header and scrambling in one pass; TS CRC-8 by table.
 *   BCH     fold 16 input bytes into the vector remainder, then reduce by
 *           bytes (t > 8) or pairs (t = 8). C/PSRAM retain nibble rows.
 *   LDPC    whole 360-bit rows: each parity row r of the q x 360 group form is
 *           a sum of byte-aligned windows of the doubled information group,
 *           shifted by 0..7 bits with a Horner scheme; then a 32x32 bit
 *           transpose and a bit packer give the natural parity order.
 *   MAP     interleaver, mapping and PL scrambling as bit-plane logic, four
 *           symbols per pair of sparse two-symbol lookups, then pilots and header.
 *
 * The C here is the portable reference of the same data layout. On the
 * ESP32-S3 (DVBS2_PIE) the hot loops can run as PIE kernels from pie.S. */
#include "dvbs2.h"
#include "private.h"
#include "fast_tables.h"

#include <string.h>

#ifdef DVBS2_IRAM
#define HOT __attribute__((section(".iram1"), noinline))
#else
#define HOT
#endif

#define ROW 48                  /* bytes per parity row: 360 bits + 24 spare */
#define U2 96                   /* bytes per doubled group: 744 bits needed */
#define GOLD 2720               /* one 8PSK Gold plane, 169 x 16 bytes rounded */

struct dvbs2_fast {
    const uint8_t *bch, *gold, *prbs, *byte_bch;
    const uint32_t *map;
    const uint16_t *prog, *cols;
    const uint8_t *crc8;
    uint8_t *p;
    // Fit 8PSK metadata in Xtensa's pre-vector alignment padding. Keep
    // common fields and scratch at their original QPSK offsets.
    const uint8_t *pair_bch;
    uint16_t lofty_history;
    uint8_t lofty_prefix, lofty_framed;
    const uint8_t *lofty_bits;
    uint8_t carry[ROW] __attribute__((aligned(16)));
    uint8_t tail[16] __attribute__((aligned(16)));
    uint8_t packed_header[48], packed_pilots[400];
    const uint8_t *header;
    const dvbs2_mode *mode;
    unsigned pilots, pie, ldxq_bias;
    unsigned bch_bytes, groups, q, data_symbols, pilot_blocks;
    uint8_t scratch[] __attribute__((aligned(16)));
};
#ifdef __XTENSA__
_Static_assert(offsetof(dvbs2_fast,scratch) == 608, "retain QPSK scratch placement");
#endif

size_t dvbs2_fast_mode_size(const dvbs2_mode *mode)
{
    if (!mode || dvbs2_find_mode(mode->modcod) != mode) {
        return 0;
    }
    const size_t ldpc = mode->nbch / 360 * U2 + ((mode->q + 31) / 32) * 32 * ROW;
    const size_t index = mode->bits_per_symbol == 3 ? 169 * 64 : 507 * 32;
    return sizeof(dvbs2_fast) + (ldpc > index ? ldpc : index);
}

size_t dvbs2_fast_size(void)
{
    size_t size = 0;
    for (size_t k = 0; k < dvbs2_mode_count; ++k) {
        const size_t n = dvbs2_fast_mode_size(&dvbs2_modes[k]);
        size = n > size ? n : size;
    }
    return size;
}

unsigned dvbs2_fast_pie(const dvbs2_fast *f)
{
    return f->pie & (DVBS2_PIE_BB | DVBS2_PIE_BCH | DVBS2_PIE_LDPC | DVBS2_PIE_MAP);
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
void dvbs2_pie_bch_regs16(const uint8_t *msg, unsigned words, const uint8_t *tables, uint8_t *r);
void dvbs2_pie_bch_regs32(const uint8_t *msg, unsigned words, const uint8_t *tables, uint8_t *r);
void dvbs2_pie_bch_byte(const uint8_t *msg, unsigned bytes, const uint8_t *tables, uint8_t *r);
void dvbs2_pie_bch_pair(const uint8_t *msg, unsigned pairs, const uint8_t *tables, uint8_t *r);
void dvbs2_pie_u2(const uint8_t *bits, uint8_t *u2, unsigned groups);
void dvbs2_pie_rows(const uint8_t *u2, const uint16_t *prog, uint8_t *p, unsigned q);
void dvbs2_pie_transpose(uint8_t *p, unsigned blocks, const uint8_t *carry);
void dvbs2_pie_index8(const uint8_t *a, const uint8_t *b, const uint8_t *c,
                      const uint8_t *gold, uint16_t *index, unsigned chunks);
void dvbs2_pie_index4(const uint8_t *x, const uint8_t *gold, uint16_t *index, unsigned chunks);
void dvbs2_pie_run8(const uint16_t *index, const uint32_t *table, uint8_t *block,
                    unsigned vectors, unsigned sar_byte, uint8_t *tail);
void dvbs2_pie_run4_packed(const uint16_t *index, const uint32_t *table, uint8_t *block,
                           unsigned vectors, unsigned sar_byte, uint8_t *tail);
void dvbs2_pie_run8_packed(const uint16_t *index, const uint32_t *table, uint8_t *block,
                           unsigned vectors, unsigned sar_byte, uint8_t *tail);
void dvbs2_pie_run(const uint16_t *index, const uint32_t *table, uint8_t *block,
                   unsigned vectors, unsigned sar_byte, uint8_t *tail);
void dvbs2_pie_pack(const uint8_t *p, const uint16_t *cols, unsigned q, uint32_t *out,
                    uint32_t pending, unsigned have);
void dvbs2_pie_bswap(const uint32_t *src, uint8_t *block, unsigned vectors, unsigned sar_byte,
                     uint8_t *tail);
uint32_t dvbs2_pie_ldxq(const uint32_t *table);
uint32_t dvbs2_pie_crc8(const uint8_t *p, unsigned quads, const uint8_t *tables, uint32_t crc);
void dvbs2_pie_nibbles(const uint8_t *src, uint8_t *dst, unsigned vectors);
#define PIE_MASK (DVBS2_PIE_BB | DVBS2_PIE_BCH | DVBS2_PIE_LDPC | DVBS2_PIE_MAP)
#else
#define PIE_MASK 0u
#endif

/* Four chronological phase>>1 codes, high pair first. Used only at setup. */
void dvbs2_pack_quads(const uint8_t *phases, uint8_t *packed, size_t symbols)
{
    for (size_t i = 0; i < symbols; i += 4) {
        unsigned v = 0;
        for (unsigned j = 0; j < 4 && i + j < symbols; ++j)
            v |= (phases[i + j] >> 1) << (6 - 2*j);
        packed[i / 4] = v;
    }
}

uint8_t *dvbs2_fast_payload(dvbs2_fast *f) { return f ? f->scratch : NULL; }

/* ---------------------------------------------------------------- prepare */

static void pack_c(const uint8_t *raw, uint8_t *packed, unsigned symbols);

static void pack_axes(uint32_t *dst, const uint8_t *phases, unsigned symbols)
{
    memset(dst,0,4*((symbols+15)/16));
    for (unsigned j = 0; j < symbols; ++j) {
        const unsigned c = phases[j] >> 1, shift = 15-(j&15);
        dst[j/16] |= ((c ^ (c >> 1))&1) << shift | (c >> 1) << (shift+16);
    }
}

static void prepare_map(dvbs2_fast *f)
{
    f->map = f->mode->bits_per_symbol == 3 ? fast_map_3 : fast_map_2;
    f->gold = f->mode->bits_per_symbol == 3
                  ? (f->pilots ? fast_gold_3_1 : fast_gold_3_0)
                  : (f->pilots ? fast_gold_2_1 : fast_gold_2_0);
    f->header = fast_headers + 90 * (2 * (f->mode->modcod - 4) + f->pilots);
    if (f->pie & DVBS2_MAP_QUADS) {
        dvbs2_pack_quads(f->header, f->packed_header, 90);
        dvbs2_pack_quads(fast_pilots, f->packed_pilots, 36 * f->pilot_blocks);
        // Unused tails retain resident axis versions without growing the context.
        pack_axes((uint32_t *)(f->packed_header+24),f->header,90);
        pack_axes((uint32_t *)(f->packed_pilots+200),fast_pilots,36*f->pilot_blocks);
        return;
    }
    pack_c(f->header, f->packed_header, 90);
    pack_c(fast_pilots, f->packed_pilots, 36 * f->pilot_blocks);
}

int dvbs2_fast_prepare(dvbs2_fast *f, const dvbs2_mode *mode, int pilots, unsigned pie)
{
    if (!f || !mode || dvbs2_find_mode(mode->modcod) != mode ||
        ((pie & DVBS2_MAP_QUADS) && mode->bits_per_symbol != 2)) {
        return -1;
    }
    f->mode = mode;
    f->pilots = pilots ? 1 : 0;
    f->bch_bytes = (mode->nbch - mode->kbch) / 8;
    f->groups = mode->nbch / 360;
    f->q = mode->q;
    f->data_symbols = 64800 / mode->bits_per_symbol;
    f->pilot_blocks = pilots ? (f->data_symbols / 90 - 1) / 16 : 0;
    f->pie = pie & (PIE_MASK | DVBS2_BCH_NIBBLE | DVBS2_MAP_SPARSE | DVBS2_MAP_QUADS | DVBS2_RO_20 | DVBS2_VCM);
    f->ldxq_bias = 0;
    f->pair_bch = NULL;
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
    f->prbs = dvbs2_prbs_table();
    f->bch = mode->t == 12 ? fast_bch_12 : mode->t == 10 ? fast_bch_10 : fast_bch_8;
    f->byte_bch = mode->t == 8 ? fast_bch_pair_8 : mode->t == 12 ? fast_bch_byte_12 : fast_bch_byte_10;
    f->cols = fast_cols;
    f->crc8 = fast_crc8;
    /* QPSK and 8PSK share their code-rate program. */
    f->prog = fast_prog[mode->bits_per_symbol == 2 ? mode->modcod - 4 :
                        (unsigned[]){1, 2, 3, 5, 6, 7}[mode->modcod - 12]];
    f->p = f->scratch + f->groups * U2;
    memset(f->scratch, 0, dvbs2_fast_mode_size(mode) - sizeof *f);
    prepare_map(f);
    return 0;
}

int dvbs2_fast_cache_bch(dvbs2_fast *f, uint8_t *table)
{
    if (!f || !f->mode || f->mode->t == 8 || ((uintptr_t)table & 15)) {
        return -1;
    }
    const uint8_t *source = f->mode->t == 12 ? fast_bch_byte_12 : fast_bch_byte_10;
    if (table) memcpy(table, source, DVBS2_BCH_CACHE_BYTES);
    f->byte_bch = table ? table : source;
    f->pair_bch = NULL;
    return 0;
}

/* T1[v] = x^8*T0[v] mod g. Only its low 128 bits need another
 * table. Its high <=64 bits fit T0's unused high-vector padding.
 * The caller has already made byte_bch resident and owns both allocations. */
int dvbs2_fast_cache_bch_pair(dvbs2_fast *f, uint8_t *low)
{
    if (!f || !f->mode || !low || ((uintptr_t)low&15) || f->bch_bytes <= 16 ||
        f->bch_bytes > 24 || f->mode->kbch%16 || (f->pie&DVBS2_BCH_NIBBLE) ||
        f->byte_bch == fast_bch_byte_12 || f->byte_bch == fast_bch_byte_10) return -1;
    uint8_t *t = (uint8_t *)f->byte_bch;
    for (unsigned v = 0; v < 256; ++v) {
        unsigned head = t[16*v];
        for (unsigned j = 0; j < 24; ++j) {
            unsigned next = j+1;
            uint8_t x = next < 16 ? t[16*v+next] : next < 24 ? t[4096+16*v+next-16] : 0;
            x ^= j < 16 ? t[16*head+j] : t[4096+16*head+j-16];
            if (j < 16) low[16*v+j] = x;
            else t[4096+16*v+8+j-16] = x;
        }
    }
    f->pair_bch = low;
    return 0;
}

/* QPSK quad streams have no mapping lookup table. Cache their complete hot
 * data set in caller-owned internal RAM; no stage then depends on flash data
 * bandwidth alongside the other core. Header/pilots already live in f. */
static size_t cache_prog_bytes(const dvbs2_fast *f)
{
    const size_t bytes[] = {sizeof fast_prog_0, sizeof fast_prog_1, sizeof fast_prog_2,
        sizeof fast_prog_3, sizeof fast_prog_4, sizeof fast_prog_5, sizeof fast_prog_6, sizeof fast_prog_7};
    const unsigned rate = f->mode->bits_per_symbol == 2 ? f->mode->modcod-4 :
        (unsigned[]){1,2,3,5,6,7}[f->mode->modcod-12];
    return (bytes[rate]+15) & ~(size_t)15;
}

static size_t cache_prbs_bytes(const dvbs2_fast *f)
{
    return (f->mode->kbch/8+15)&~(size_t)15;
}

size_t dvbs2_fast_cache_size(const dvbs2_fast *f)
{
    if (!f || !f->mode) return 0;
    return cache_prbs_bytes(f)+(f->mode->bits_per_symbol == 3 ? sizeof fast_gold_3_1 : sizeof fast_gold_2_1)+
        DVBS2_BCH_CACHE_BYTES+cache_prog_bytes(f)+sizeof fast_cols+256;
}

size_t dvbs2_fast_cache_tail_size(const dvbs2_fast *f)
{
    return f && f->mode ? cache_prog_bytes(f)+sizeof fast_cols+256 : 0;
}

int dvbs2_fast_cache_split(dvbs2_fast *f, uint8_t *table, uint8_t *tail)
{
    if (!dvbs2_fast_cache_size(f) || !table || ((uintptr_t)table & 15) || ((uintptr_t)tail & 15)) return -1;
    memcpy(table,f->prbs,f->mode->kbch/8);
    f->prbs = table; table += cache_prbs_bytes(f);
    size_t gold = f->mode->bits_per_symbol == 3 ? sizeof fast_gold_3_1 : sizeof fast_gold_2_1;
    memcpy(table,f->gold,gold);
    f->gold = table; table += gold;
    if (f->pie & DVBS2_BCH_NIBBLE) {
        memcpy(table,f->bch,sizeof fast_bch_8);
        f->bch = table;
    } else {
        memcpy(table,f->byte_bch,DVBS2_BCH_CACHE_BYTES);
        f->byte_bch = table;
    }
    table += DVBS2_BCH_CACHE_BYTES;
    if (tail) table = tail;
    size_t bytes = cache_prog_bytes(f);
    // Copy only the real program; its aligned slot may be longer.
    const uint16_t *end = f->prog;
    for (unsigned row = 0; row < f->q; ++row) {
        unsigned levels = *end++;
        while (levels--) { unsigned n = *end++; end += n; }
    }
    memcpy(table,f->prog,(const uint8_t *)end-(const uint8_t *)f->prog);
    f->prog = (const uint16_t *)table; table += bytes;
    memcpy(table,f->cols,sizeof fast_cols);
    f->cols = (const uint16_t *)table; table += sizeof fast_cols;
    memcpy(table,f->crc8,256);
    f->crc8 = table;
    return 0;
}

int dvbs2_fast_cache_tables(dvbs2_fast *f, uint8_t *table)
{
    return dvbs2_fast_cache_split(f,table,NULL);
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
    bb_header(ws->bits, f->crc8, ((f->pie & DVBS2_VCM) ? 0x60 : 0x70) |
              ((f->pie & DVBS2_RO_20) ? 2 : 0), (unsigned)dfl_bits, 0, 0, 0);
    bb_fill(f, ws->bits, payload, n);
    if (dfl_bits & 7) {
        /* Clear the unused low bits of the last data byte, then rescramble. */
        const size_t last = 10 + dfl_bits / 8;
        const uint8_t keep = (uint8_t)(0xff << (8 - (dfl_bits & 7)));
        ws->bits[last] = (uint8_t)(((ws->bits[last] ^ f->prbs[last]) & keep) ^ f->prbs[last]);
    }
    return 0;
}

int dvbs2_fast_bb_ts(const dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *state,
                         const uint8_t *input, size_t input_bytes)
{
    if (!f || !ws || !state || !input || (f->pie & DVBS2_VCM) || state->packet_pos >= 188 ||
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
    /* LDPC parity scratch is idle until BB and BCH finish. Keep the four CRC-8
     * tables here during TS adaptation, away from the flash mapper/PRBS lines.
     * The borrowed payload occupies the start of scratch, before f->p. */
    memcpy(f->p, fast_crc8, sizeof fast_crc8);
    const uint8_t *crc8 = f->p;
    bb_header(ws->bits, crc8, 0xf0, (unsigned)(input_bytes * 8), 1504, 0x47, syncd);
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
        const uint8_t *t0 = crc8, *t1 = crc8 + 256, *t2 = crc8 + 512, *t3 = crc8 + 768;
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

/* Keep the six remainder words in registers. A loop over remainder words
 * makes the compiler reload them for every slice on Xtensa. */
#define BCH_SLICE(k, shift) do { \
    const uint8_t *row = t + 512 * (k) + (((x >> (shift)) & 15) << 4); \
    w0 ^= W(row); w1 ^= W(row + 4); w2 ^= W(row + 8); w3 ^= W(row + 12); \
    w4 ^= W(row + 256); w5 ^= W(row + 260); \
} while (0)

static __attribute__((noinline)) void bch_c(const dvbs2_fast *f, const uint8_t *bits, unsigned words, uint32_t r[8])
{
    const uint8_t *t = f->bch;
    uint32_t w0 = r[0], w1 = r[1], w2 = r[2], w3 = r[3], w4 = r[4], w5 = r[5];
    for (unsigned i = 0; i < words; ++i) {
        const uint32_t x = w0 ^ W(bits + 4 * i);
        w0 = w1; w1 = w2; w2 = w3; w3 = w4; w4 = w5; w5 = 0;
        BCH_SLICE(0, 4); BCH_SLICE(1, 0);
        BCH_SLICE(2, 12); BCH_SLICE(3, 8);
        BCH_SLICE(4, 20); BCH_SLICE(5, 16);
        BCH_SLICE(6, 28); BCH_SLICE(7, 24);
    }
    r[0] = w0; r[1] = w1; r[2] = w2; r[3] = w3; r[4] = w4; r[5] = w5;
}
#undef BCH_SLICE

HOT void dvbs2_fast_bch(dvbs2_fast *f, dvbs2_workspace *ws)
{
    const unsigned bytes = f->mode->kbch / 8;
    unsigned words = bytes / 4;
    uint32_t w[8] __attribute__((aligned(16))) = {0};
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_BCH) {
        if (f->bch_bytes == 16 && !(f->pie & DVBS2_BCH_NIBBLE)) {
            dvbs2_pie_bch_pair(ws->bits, bytes/2, f->byte_bch, (uint8_t *)w);
            words = (bytes + 3) / 4;
        } else if (f->bch_bytes > 16 && !(f->pie & DVBS2_BCH_NIBBLE)) {
            if (f->pair_bch) {
                void dvbs2_pie_bch_pair32(const uint8_t *, unsigned, const uint8_t *, const uint8_t *, uint8_t *);
                dvbs2_pie_bch_pair32(ws->bits,bytes/2,f->byte_bch,f->pair_bch,(uint8_t *)w);
            } else dvbs2_pie_bch_byte(ws->bits, bytes, f->byte_bch, (uint8_t *)w);
            words = (bytes + 3) / 4; /* byte kernel already consumed the tail */
        } else {
            (f->bch_bytes > 16 ? dvbs2_pie_bch_regs32 : dvbs2_pie_bch_regs16)(
                ws->bits, words, f->bch, (uint8_t *)w);
        }
    } else
#endif
    {
        if (f->pair_bch) {
            uint8_t *r = (uint8_t *)w;
            for (unsigned i = 0; i < bytes; i += 2) {
                unsigned a = r[0]^ws->bits[i], b = r[1]^ws->bits[i+1];
                for (unsigned j = 0; j < 24; ++j)
                    r[j] = (j+2 < 24 ? r[j+2] : 0) ^
                        (j < 16 ? f->pair_bch[16*a+j] : f->byte_bch[4096+16*a+8+j-16]) ^
                        f->byte_bch[j < 16 ? 16*b+j : 4096+16*b+j-16];
            }
            words = (bytes+3)/4;
        } else bch_c(f, ws->bits, words, w);
    }
    /* A final partial word: two nibble-table steps per remaining byte. */
    uint8_t *r = (uint8_t *)w;
    for (unsigned i = 4 * words; i < bytes; ++i) {
        for (int shift = 4; shift >= 0; shift -= 4) {
            const unsigned v = (r[0] >> 4) ^ ((ws->bits[i] >> shift) & 15);
            const uint8_t *row = f->bch + 7 * 512 + 16 * v;
            for (unsigned j = 0; j < f->bch_bytes; ++j) {
                r[j] = (uint8_t)((r[j] << 4) | (r[j + 1] >> 4));
                r[j] ^= row[j < 16 ? j : 256 + j - 16];
            }
        }
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

static __attribute__((noinline)) void rows_c(dvbs2_fast *f)
{
    const uint16_t *prog = f->prog;
    uint8_t *p = f->p;
    for (unsigned r = 0; r < f->q; ++r) {
        uint32_t h[12] = {0};
        const unsigned levels = *prog++;
        for (unsigned level = 0; level < levels; ++level) {
            if (level) {
                shift_1(h);
            }
            for (unsigned n = *prog++; n; --n) {
                const unsigned offset = *prog++, shift = 8 * (offset & 3);
                const uint8_t *window = f->scratch + (offset & ~3u);
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

static __attribute__((noinline)) void transpose_c(dvbs2_fast *f, unsigned blocks)
{
    uint8_t *p = f->p;
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
static __attribute__((noinline)) void carry_row(dvbs2_fast *f)
{
    const uint8_t *last = f->p + ROW * (f->q - 1);
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

static __attribute__((noinline)) void pack(dvbs2_fast *f, uint8_t *bits)
{
    const unsigned q = f->q, start = f->mode->nbch / 8, full = q / 32, part = q % 32;
    const uint8_t *p = f->p;
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
        dvbs2_pie_u2(ws->bits, f->scratch, f->groups);
        dvbs2_pie_rows(f->scratch, f->prog, f->p, f->q);
        carry_row(f);
        dvbs2_pie_transpose(f->p, blocks, f->carry);
        /* Pack big-endian words into the free u2 area, then reverse their
         * bytes into ws->bits from the word holding the parity start. */
        const unsigned start = f->mode->nbch / 8, base = start - start % 4;
        const unsigned words = (DVBS2_FEC_BYTES - base) / 4, vectors = words / 4;
        uint32_t pending;
        const unsigned have = pack_start(ws->bits, start, &pending);
        uint32_t *packed = (uint32_t *)f->scratch;
        dvbs2_pie_pack(f->p, f->cols, f->q, packed, pending, have);
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
        uint8_t *u2 = f->scratch + U2 * g;
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
/* Data symbols [k0, k0 + n) to dst; n is a multiple of 8 (8PSK) or 4 (QPSK).
 * last: the frame's final run, whose trailing bytes go one by one. */
static __attribute__((noinline)) void map_c_run(const dvbs2_fast *f, const uint8_t *bits, unsigned k0, unsigned n,
                          uint8_t *dst, int last)
{
    const uint32_t *map = f->map;
    writer w = writer_start(dst);
    if (f->mode->bits_per_symbol == 3) {
        const uint8_t *a = bits, *b = bits + 2700, *c = bits + 5400;
        if (f->mode->modcod == 12) {   /* 5.3.3: rate 3/5 reads the columns backwards */
            a = bits + 5400;
            c = bits;
        }
        for (unsigned j = k0 / 8; j < (k0 + n) / 8; ++j) {
            const unsigned x = b[j] ^ c[j], z0 = f->gold[j], z1 = f->gold[GOLD + j];
            const unsigned p0 = a[j] ^ x;                       /* inverted p0 */
            const unsigned p1 = a[j] ^ z0;
            const unsigned p2 = b[j] ^ (a[j] & x) ^ z1 ^ (a[j] & z0);
            for (int shift = 6; shift >= 0; shift -= 4) {
                const unsigned hi = ((p0 >> shift) & 3) | (((p2 >> shift) & 3) << 4) |
                                    (((p1 >> shift) & 3) << 8);
                const unsigned lo = ((p0 >> (shift - 2)) & 3) |
                                    (((p2 >> (shift - 2)) & 3) << 4) |
                                    (((p1 >> (shift - 2)) & 3) << 8);
                writer_put(&w, map[hi] | (map[lo] << 16));
            }
        }
    } else {
        for (unsigned j = k0 / 4; j < (k0 + n) / 4; ++j) {
            const unsigned x = bits[j], g = f->gold[j];
            const unsigned dd = x ^ (x >> 1);
            const unsigned p2 = x ^ (g >> 1) ^ (dd & g), p1 = dd ^ g;
            writer_put(&w, map[(p1 & 0x55) | (p2 & 0x55) << 1]);
        }
    }
    writer_end(&w, last);
}

static __attribute__((noinline)) void map_c(const dvbs2_fast *f, const uint8_t *bits, uint8_t *output)
{
    const unsigned d = f->data_symbols, runs = f->pilots ? f->pilot_blocks + 1 : 1;
    const unsigned run_symbols = f->pilots ? 1440 : d;
    for (unsigned r = 0; r < runs; ++r) {
        const unsigned k0 = r * run_symbols;
        const unsigned n = d - k0 < run_symbols ? d - k0 : run_symbols;
        map_c_run(f, bits, k0, n, output + 90 + k0 + 36 * r, r == runs - 1);
    }
}

HOT void dvbs2_fast_pl(const dvbs2_fast *f, uint8_t *output)
{
    memcpy(output, f->header, 90);
    for (unsigned b = 0; b < f->pilot_blocks; ++b) {
        memcpy(output + 90 + 1440 * (b + 1) + 36 * b, fast_pilots + 36 * b, 36);
    }
}

#ifdef DVBS2_PIE
/* Index pass over the whole frame, then one shifted-store pass per run. */
static HOT void map_pie(dvbs2_fast *f, const uint8_t *bits, uint8_t *output)
{
    const unsigned d = f->data_symbols;
    uint16_t *index = (uint16_t *)f->scratch;
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
        (f->mode->bits_per_symbol == 3 ? dvbs2_pie_run8 : dvbs2_pie_run)(
            index + k / 4, table, block, vectors, (unsigned)(-at & 15), f->tail);
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

/* ---------------------------------------------------------------- packed */

/* Two symbols per byte. Direct PIE mapping uses an 8 KiB table; MAP_SPARSE
 * keeps main's 512 B of hot mapping lines for streams with a PSRAM workspace.
 * Its temporary phases reuse existing scratch after the index area. */
#define PACK_RUN 1440
#define PACK_TEMP (169 * 64 + 16)   /* after the index; run8 writes 16 bytes before dst */

static HOT void pack_c(const uint8_t *raw, uint8_t *packed, unsigned symbols)
{
    for (unsigned i = 0; i < symbols / 2; ++i) {   /* forward: packed may equal raw */
        packed[i] = (uint8_t)(raw[2 * i] | raw[2 * i + 1] << 4);
    }
}

/* Keep the fallback's control loop in flash, so restoring it adds no internal
 * instruction RAM. The vector kernels and their existing scratch stay as before.
 * The PIE index pass has already run when this is called. */
static __attribute__((noinline)) int map_packed_runs(dvbs2_fast *f,
                                                   const dvbs2_workspace *ws, uint8_t *packed)
{
    uint8_t *temp = f->scratch + PACK_TEMP;
#ifdef DVBS2_PIE
    const int pie = (f->pie & DVBS2_PIE_MAP) != 0;
    uint16_t *index = (uint16_t *)f->scratch;
    const uint32_t *table = (const uint32_t *)((const uint8_t *)f->map + f->ldxq_bias);
#endif
    for (unsigned k = 0; k < f->data_symbols; k += PACK_RUN) {
        const unsigned left = f->data_symbols - k;
        const unsigned n = left < PACK_RUN ? left : PACK_RUN;
#ifdef DVBS2_PIE
        if (pie && f->mode->bits_per_symbol == 3) {
            dvbs2_pie_run8(index + k / 4, table, temp - 16, PACK_RUN / 16, 0, f->tail);
            memcpy(temp + PACK_RUN - 16, f->tail, 16);
            dvbs2_pie_nibbles(temp, temp, PACK_RUN / 32);
        } else
#endif
        {
            map_c_run(f, ws->bits, k, n, temp, 0);
            pack_c(temp, temp, n);
        }
        const unsigned at = 90 + k + (f->pilots ? 36 * (k / 1440) : 0);
        memcpy(packed + at / 2, temp, n / 2);
    }
    return 0;
}

/* Four input bytes make sixteen QPSK symbols. The bit-plane logic already
 * gives chronological two-bit codes: no index array, lookup or repacking pass.
 * Every data run begins halfway through an output byte (90 + 1440r + 36r).
 * Shift by a nibble and let PL framing replace the header/pilot halves. */
static HOT int map_quads(dvbs2_fast *f, const uint8_t *bits, uint8_t *packed)
{
#ifdef DVBS2_PIE
    const uint8_t *mapped = NULL;
    if (f->pie & DVBS2_PIE_MAP) {
        void dvbs2_pie_quads(const uint8_t *, const uint8_t *, uint8_t *, unsigned);
        dvbs2_pie_quads(bits, f->gold, f->scratch, (f->data_symbols+63)/64);
        mapped = f->scratch;
    }
#endif
    const unsigned runs = f->pilots ? f->pilot_blocks + 1 : 1;
    for (unsigned r = 0; r < runs; ++r) {
        const unsigned k = f->pilots ? r * PACK_RUN : 0;
        const unsigned left = f->data_symbols - k;
        const unsigned n = f->pilots && left > PACK_RUN ? PACK_RUN : left;
        uint8_t *dst = packed + (90 + k + 36*r) / 4;
        unsigned first = k/4;
        uint32_t previous = 0;
#ifdef DVBS2_PIE
        if (mapped) {
            void dvbs2_pie_quad_run(const uint8_t *, uint8_t *, unsigned, unsigned, uint8_t *);
            const unsigned vectors = n/64;
            const uintptr_t at = (uintptr_t)dst;
            uint8_t *block = (uint8_t *)((at - 1) & ~(uintptr_t)15);
            dvbs2_pie_quad_run(mapped + first, block, vectors, (unsigned)(-at & 15), f->tail);
            memcpy(block + 16*vectors, f->tail, (at & 15) ? (at & 15) : 16);
            first += 16*vectors;
            previous = (mapped[first-1] & 15) << 4;
            for (unsigned j = first; j < (k+n)/4; ++j) {
                dst[j-k/4] = previous | (mapped[j] >> 4);
                previous = (mapped[j] & 15) << 4;
            }
            dst[n/4] = previous;
            continue;
        }
#endif
        writer w = writer_start(dst);
        for (unsigned j = first; j < (k+n)/4; j += 4) {
            const uint32_t x = W(bits+j), g = W(f->gold+j), dd = x ^ (x >> 1);
            const uint32_t p2 = x ^ (g >> 1) ^ (dd & g), p1 = dd ^ g;
            const uint32_t v = (p1 & 0x55555555) | ((p2 & 0x55555555) << 1);
            writer_put(&w, ((v >> 4) & 0x0f0f0f0f) | ((v & 0x000f0f0f) << 12) | previous);
            previous = (v >> 20) & 0xf0;
        }
        writer_end(&w, 1);
        dst[n/4] = previous;
    }
    return 0;
}

HOT int dvbs2_fast_sign_start(dvbs2_fast *f, const dvbs2_workspace *ws, dvbs2_sign_state *s)
{
    if (!f || !ws || !s || f->mode->bits_per_symbol != 2) return -1;
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_MAP) {
        void dvbs2_pie_quads(const uint8_t *, const uint8_t *, uint8_t *, unsigned);
        dvbs2_pie_quads(ws->bits, f->gold, f->scratch, (f->data_symbols+63)/64);
    } else
#endif
    {
        for (unsigned j = 0; j < f->data_symbols/4; ++j) {
            const unsigned x = ws->bits[j], g = f->gold[j], d = x ^ (x >> 1);
            const unsigned p2 = x ^ (g >> 1) ^ (d & g), p1 = d ^ g;
            f->scratch[j] = (p1 & 0x55) | ((p2 & 0x55) << 1);
        }
    }
    s->at = 0;
    return 0;
}

int dvbs2_fast_lofty_cache(dvbs2_fast *f, uint32_t *table)
{
    if (!f || !table || ((uintptr_t)table&15) || f->mode->bits_per_symbol != 3) return -1;
    // One byte -> two chronological four-phase words, one phase bit each.
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t v = 0;
        for (unsigned j = 0; j < 8; ++j)
            v |= ((i>>(7-j))&1)<<(16*(j/4)+9-3*(j&3));
        table[i] = v;
    }
    uint16_t *header = (uint16_t *)f->packed_header, *pilots = (uint16_t *)f->packed_pilots;
    memset(header,0,sizeof f->packed_header);
    memset(pilots,0,sizeof f->packed_pilots);
    for (unsigned i = 0; i < 90; ++i) header[i/4] |= f->header[i]<<(9-3*(i&3));
    for (unsigned i = 0; i < 36*f->pilot_blocks; ++i) pilots[i/4] |= fast_pilots[i]<<(9-3*(i&3));
    f->map = table;
    return 0;
}

/* A run is staged beyond the framed words before expansion overwrites it.
 * PIE stores may touch the two aligned boundary vectors, both context-owned;
 * restore their unused bytes. The portable path uses memmove. */
static void lofty_store(uint16_t *dst, const uint16_t *src, unsigned words)
{
#ifdef DVBS2_PIE
    uint32_t dvbs2_pie_lofty_copy(uint16_t *, const uint16_t *, unsigned, unsigned, uint32_t);
    void dvbs2_pie_lofty_store(const uint16_t *, uint8_t *, unsigned, unsigned, uint8_t *);
    unsigned prefix = (uintptr_t)dst&15;
    if (!prefix) dvbs2_pie_lofty_copy(dst,src,words/8,0,0);
    else {
        uint8_t keep[16], tail[16] __attribute__((aligned(16)));
        uint8_t *block = (uint8_t *)dst-prefix;
        memcpy(keep,block,prefix);
        dvbs2_pie_lofty_store(src,block,words/8,16-prefix,tail);
        memcpy(block,keep,prefix);
        memcpy(block+2*words,tail,prefix);
    }
#else
    memmove(dst,src,2*words);
#endif
}

/* Insert a short four-phase PL run without touching either neighbouring half.
 * The input and output are halfword aligned. The final source word is padded. */
static void lofty_insert(uint16_t *dst, unsigned at, const uint16_t *src, unsigned symbols)
{
    if (!(at&2)) {
        memcpy(dst+at/4,src,2*(symbols/4));
        if (symbols&2) dst[(at+symbols)/4] = (dst[(at+symbols)/4]&63) | src[symbols/4];
    } else {
        unsigned words = symbols/4;
        uint32_t h = dst[at/4]>>6;
        for (unsigned j = 0; j < words; ++j) {
            uint32_t w = src[j];
            dst[at/4+j] = ((h&63)<<6) | (w>>6);
            h = w;
        }
        unsigned last = at/4+words;
        dst[last] = ((h&63)<<6) | ((symbols&2) ? src[words]>>6 : dst[last]&63);
    }
}

static uint16_t *lofty_temp(dvbs2_fast *f)
{
    unsigned end = 90+f->data_symbols+36*f->pilot_blocks;
    return (uint16_t *)(f->scratch+((2*((end+5)/4)+15)&~15u));
}

// One chunk is 128 symbols. Map the first 512 symbols before publication,
// then defer the rest until the first page is visible to the consumer.
static void lofty_map(dvbs2_fast *f, unsigned first, unsigned chunks)
{
    const uint8_t *a = f->lofty_bits, *b = a+2700, *c = a+5400;
    if (f->mode->modcod == 12) { a = f->lofty_bits+5400; c = f->lofty_bits; }
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_MAP) {
        void dvbs2_pie_lofty(const uint8_t *const *, const uint8_t *, uint16_t *,
                             const uint32_t *, unsigned);
        const uint8_t *planes[] = {a+16*first,b+16*first,c+16*first};
        const uint32_t *table = (const uint32_t *)((const uint8_t *)f->map+f->ldxq_bias);
        dvbs2_pie_lofty(planes,f->gold+16*first,(uint16_t *)f->scratch+32*first,table,chunks);
        return;
    }
#endif
    uint16_t *out = (uint16_t *)f->scratch+32*first;
    unsigned end = 16*(first+chunks);
    if (end > f->data_symbols/8) end = f->data_symbols/8;
    for (unsigned j = 16*first; j < end; ++j) {
        unsigned x = b[j]^c[j], p0 = a[j]^x, p1 = a[j]^f->gold[j];
        unsigned p2 = b[j]^(a[j]&x)^f->gold[GOLD+j]^(a[j]&f->gold[j]);
        uint32_t v = f->map[(~p0)&255] | (f->map[p1&255]<<1) | (f->map[p2&255]<<2);
        *out++ = v;
        *out++ = v>>16;
    }
}

static void lofty_frame(dvbs2_fast *f)
{
    lofty_map(f,4,164); // tail chunk 168 was mapped at start for frame history
    // Expand backwards so each source run is consumed before framing overwrites it.
    // One 720 B run buffer fits after the framed words in every mode's scratch.
    uint16_t *mapped = (uint16_t *)f->scratch;
    uint16_t history = f->lofty_history;
    unsigned offset = (f->lofty_prefix+90)&3;
    uint16_t *temp = lofty_temp(f);
    const unsigned runs = f->data_symbols/1440;
    for (unsigned run = runs; run-- > 0;) {
        unsigned k = 1440*run, n = 1440;
        uint16_t *dst = mapped+(f->lofty_prefix+90+k+(f->pilots ? 36*run : 0))/4;
        const uint16_t *src = mapped+k/4;
#ifdef DVBS2_PIE
        if (f->pie & DVBS2_PIE_MAP) {
            // Move backwards once, including the optional two-symbol shift.
            // The source's readable preceding vector is inside f; its first
            // halfword is replaced by PL framing below. Keep both output edges.
            void dvbs2_pie_lofty_move(uint16_t *, const uint16_t *, unsigned, unsigned);
            unsigned prefix = (uintptr_t)dst&15;
            uint8_t head[16], tail[16];
            uint16_t last = src[n/4-1];
            if (prefix) {
                memcpy(head,(uint8_t *)dst-prefix,prefix);
                memcpy(tail,dst+n/4,16-prefix);
            }
            dvbs2_pie_lofty_move(dst,src,n/32,offset);
            if (prefix) {
                memcpy((uint8_t *)dst-prefix,head,prefix);
                memcpy(dst+n/4,tail,16-prefix);
            }
            if (offset) dst[n/4] = ((last&63)<<6) | (dst[n/4]&63);
            continue;
        }
#endif
        if (!offset) {
            memmove(dst,src,n/2);
        } else {
            uint16_t last = src[n/4-1];
            {
                uint32_t h = 0;
                for (unsigned j = 0; j < n/4; ++j) {
                    uint32_t w = src[j];
                    temp[j] = ((h&63)<<6) | (w>>6);
                    h = w;
                }
            }
            // The first/last halfwords border framing, filled below.
            lofty_store(dst,temp,n/4);
            dst[n/4] = ((last&63)<<6) | (dst[n/4]&63);
        }
    }
    if (f->lofty_prefix) mapped[0] = (history&63)<<6;
    lofty_insert(mapped,f->lofty_prefix,(const uint16_t *)f->packed_header,90);
    for (unsigned run = 0; run < f->pilot_blocks; ++run)
        lofty_insert(mapped,f->lofty_prefix+90+1440*(run+1)+36*run,
                     (const uint16_t *)f->packed_pilots+9*run,36);
    f->lofty_framed = 2;
}

/* Publish a first page before bulk mapping and expansion. */
static const uint16_t *lofty_head(dvbs2_fast *f)
{
    uint16_t *temp = lofty_temp(f), *head = temp+360;
    unsigned offset = (f->lofty_prefix+90)&3;
    memset(head,0,320);
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_MAP) {
        uint32_t dvbs2_pie_lofty_copy(uint16_t *, const uint16_t *, unsigned, unsigned, uint32_t);
        dvbs2_pie_lofty_copy(temp,(const uint16_t *)f->scratch,16,offset,0);
    } else
#endif
    {
        const uint16_t *src = (const uint16_t *)f->scratch;
        uint32_t h = 0;
        for (unsigned j = 0; j < 128; ++j) {
            uint32_t w = src[j];
            temp[j] = offset ? ((h&63)<<6) | (w>>6) : w;
            h = w;
        }
    }
    lofty_store(head+(f->lofty_prefix+90)/4,temp,128);
    if (f->lofty_prefix) head[0] = (f->lofty_history&63)<<6;
    lofty_insert(head,f->lofty_prefix,(const uint16_t *)f->packed_header,90);
    f->lofty_framed = 1;
    return head;
}

__attribute__((noinline)) int dvbs2_fast_lofty_start(dvbs2_fast *f, const dvbs2_workspace *ws, dvbs2_sign_state *s)
{
    if (!f || !ws || !s || f->mode->bits_per_symbol != 3) return -1;
    f->lofty_bits = ws->bits;
    lofty_map(f,0,4);
    // Preserve the next frame's history without mapping the middle yet.
    lofty_map(f,168,1);
    const unsigned end = 90+f->data_symbols+36*f->pilot_blocks;
    f->lofty_prefix = s->pending;
    f->lofty_history = s->history;
    f->lofty_framed = 0;
    s->history = ((const uint16_t *)f->scratch)[f->data_symbols/4-1];
    s->pending = (s->pending+end)&3;
    s->at = 0;
    return 0;
}

const uint16_t *dvbs2_fast_lofty_read(dvbs2_fast *f, dvbs2_sign_state *s, unsigned *blocks)
{
    const unsigned end = 90+f->data_symbols+36*f->pilot_blocks;
    unsigned at = (s->at+f->lofty_prefix)/4, words = (end+f->lofty_prefix)/4;
    unsigned n = words-at < *blocks ? words-at : *blocks;
    const uint16_t *src = (const uint16_t *)f->scratch+at;
    if (n && !f->lofty_framed) {
        if (n > 128) n = 128;
        src = lofty_head(f);
    } else if (n && f->lofty_framed == 1) lofty_frame(f);
    *blocks = n;
    if (n) {
        at += n;
        s->at = at == words ? end : 4*at-f->lofty_prefix;
    }
    return src;
}

HOT unsigned dvbs2_fast_sign_read(const dvbs2_fast *f, dvbs2_sign_state *s, uint32_t *dst,
                                  unsigned blocks, const uint32_t *lut)
{
    unsigned done = 0;
    const unsigned end = 90+f->data_symbols+36*f->pilot_blocks;
    while (done < blocks && s->at < end) {
        const uint8_t *src;
        unsigned at, n, packed = 1;
        if (s->at < 90) {
            packed = !!(f->pie & DVBS2_MAP_QUADS);
            src = packed ? f->packed_header : f->header;
            at = s->at;
            n = 90-at;
        } else {
            const unsigned k = s->at-90;
            const unsigned run = f->pilots ? k/1476 : 0;
            const unsigned pos = k-1476*run;
            if (pos >= 1440 && run < f->pilot_blocks) {
                packed = !!(f->pie & DVBS2_MAP_QUADS);
                src = packed ? f->packed_pilots : fast_pilots;
                at = 36*run+pos-1440;
                n = 1476-pos;
            } else {
                src = f->scratch;
                at = k-36*run;
                const unsigned left = f->data_symbols-at;
                n = f->pilots && left > 1440-pos ? 1440-pos : left;
            }
        }
        const unsigned room = (blocks-done)*5-s->pending;
        if (n > room) n = room;
        // Stay inside one header/data/pilot span. Do not divide the frame
        // position and spill the rolling state again for every edge symbol.
        uint32_t history = s->history;
        unsigned pending = s->pending, used = 0;
        while (used < n) {
#ifdef DVBS2_PIE
            // Both C and PIE starts produce the same quad bytes. The scalar
            // sign kernel needs no PIE mapping flag; keep main's hot loop.
            if (packed && !pending && !(at&3) && n-used >= 20) {
                const unsigned runs = (n-used)/20;
                uint32_t dvbs2_pie_signs(uint32_t *, const uint8_t *, unsigned,
                                        const uint32_t *, uint32_t);
                history = dvbs2_pie_signs(dst+done,src+at/4,runs,lut,history);
                at += 20*runs;
                used += 20*runs;
                done += 4*runs;
                continue;
            }
#endif
            // Five symbols always fit in two bytes, at every pair offset.
            // This handles page/PL edges without five scalar history updates.
            if (packed && !pending && n-used >= 5) {
                const unsigned w = (unsigned)src[at/4] << 8 | src[at/4+1];
                const unsigned index = (w >> (6-2*(at&3))) & 1023;
                history = ((history << 5) & 0x7ff07ff0) | lut[index];
                dst[done++] = history;
                at += 5, used += 5;
                continue;
            }
            const unsigned c = packed ? (src[at/4] >> (6-2*(at&3))) & 3 : src[at] >> 1;
            history = ((history << 1) & 0x7ff07ff0) |
                      ((c ^ (c >> 1)) & 1) << 4 | (c >> 1) << 20;
            ++at, ++used;
            if (++pending == 5) {
                dst[done++] = history;
                pending = 0;
            }
        }
        s->history = history;
        s->pending = pending;
        s->at += used;
    }
    return done;
}

/* Axis pages leave the five-symbol window expansion on the renderer core.
 * Plane words have sixteen chronological signs per axis, oldest at bits 15/31. */
HOT int dvbs2_fast_axis_start(dvbs2_fast *f, const dvbs2_workspace *ws, dvbs2_sign_state *s)
{
    if (!f || !(f->pie & DVBS2_MAP_QUADS)) return -1;
    if (dvbs2_fast_sign_start(f,ws,s)) return -1;
#ifdef DVBS2_PIE
    if (f->pie & DVBS2_PIE_MAP) {
        void dvbs2_qpsk_planes(uint8_t *, unsigned);
        dvbs2_qpsk_planes(f->scratch,(f->data_symbols+63)/64);
        return 0;
    }
#endif
    for (unsigned j = 0; j < f->data_symbols/16; ++j) {
        uint32_t i = 0, q = 0;
        for (unsigned b = 0; b < 4; ++b) {
            const unsigned w = f->scratch[4*j+b];
            for (int shift = 6; shift >= 0; shift -= 2) {
                const unsigned c = w >> shift;
                i = (i << 1) | ((c ^ (c >> 1))&1);
                q = (q << 1) | ((c >> 1)&1);
            }
        }
        W(f->scratch+4*j) = i | (q << 16);
    }
    return 0;
}

HOT unsigned dvbs2_fast_axis_read(const dvbs2_fast *f, dvbs2_sign_state *s, uint32_t *dst,
                                  unsigned blocks)
{
    unsigned done = 0;
    const unsigned end = 90+f->data_symbols+36*f->pilot_blocks;
    while (done < blocks && s->at < end) {
        const uint8_t *src;
        unsigned at, n;
        if (s->at < 90) {
            src = f->packed_header+24;
            at = s->at;
            n = 90-at;
        } else {
            const unsigned k = s->at-90;
            const unsigned run = f->pilots ? k/1476 : 0;
            const unsigned pos = k-1476*run;
            if (pos >= 1440 && run < f->pilot_blocks) {
                src = f->packed_pilots+200;
                at = 36*run+pos-1440;
                n = 1476-pos;
            } else {
                src = f->scratch;
                at = k-36*run;
                const unsigned left = f->data_symbols-at;
                n = f->pilots && left > 1440-pos ? 1440-pos : left;
            }
        }
        const unsigned room = (blocks-done)*16-s->pending;
        if (n > room) n = room;
        uint32_t history = s->history;
        unsigned pending = s->pending, used = 0;
        while (used < n) {
            if (!pending && n-used >= 16) {
                const unsigned words = (n-used)/16;
                const uint32_t *p = (const uint32_t *)(src+4*(at/16));
                const unsigned off = at&15;
                if (!off) {
                    memcpy(dst+done,p,4*words);
                } else {
#ifdef DVBS2_PIE
                    if (f->pie & DVBS2_PIE_MAP) {
                        void dvbs2_qpsk_shift(uint32_t *, const uint32_t *, unsigned, unsigned);
                        dvbs2_qpsk_shift(dst+done,p,words,off);
                    } else
#endif
                    {
                        uint32_t prev = *p++;
                        for (unsigned j = 0; j < words; ++j) {
                            const uint32_t next = *p++;
                            const uint32_t i = ((prev << off) | ((next & 0xffff) >> (16-off))) & 0xffff;
                            const uint32_t q = ((prev >> 16) << off) | (next >> (32-off));
                            dst[done+j] = i | (q << 16);
                            prev = next;
                        }
                    }
                }
                history = dst[done+words-1];
                at += 16*words, used += 16*words, done += words;
                continue;
            }
            // Append a whole fragment at a PL/page edge, never one symbol at a time.
            const unsigned off = at&15;
            unsigned take = 16-pending;
            if (take > n-used) take = n-used;
            if (take > 16-off) take = 16-off;
            const uint32_t w = W(src+4*(at/16));
            const uint32_t mask = (1u << take)-1;
            const unsigned shift = 16-off-take;
            const uint32_t i = (w >> shift)&mask, q = (w >> (shift+16))&mask;
            const uint32_t old = ((history << take) & (0xffff ^ mask)) |
                                 (((history >> 16) << take) << 16);
            history = old | i | (q << 16);
            at += take, used += take, pending += take;
            if (pending == 16) {
                dst[done++] = history;
                pending = 0;
            }
        }
        s->history = history;
        s->pending = pending;
        s->at += used;
    }
    return done;
}

HOT int dvbs2_fast_map_packed(dvbs2_fast *f, const dvbs2_workspace *ws, uint8_t *packed)
{
    if (!f || !ws || !packed) return -1;
    if (f->pie & DVBS2_MAP_QUADS) return map_quads(f, ws->bits, packed);
#ifdef DVBS2_PIE
    const int pie = (f->pie & DVBS2_PIE_MAP) != 0;
    uint16_t *index = (uint16_t *)f->scratch;
    if (pie) {
        if (f->mode->bits_per_symbol == 3) {
            const uint8_t *a = ws->bits, *c = ws->bits + 5400;
            if (f->mode->modcod == 12) {
                a = ws->bits + 5400;
                c = ws->bits;
            }
            dvbs2_pie_index8(a, ws->bits + 2700, c, f->gold, index, (f->data_symbols + 127) / 128);
        } else {
            dvbs2_pie_index4(ws->bits, f->gold, index, (f->data_symbols + 63) / 64);
        }
    }
    if (pie && (f->mode->bits_per_symbol == 2 || !(f->pie & DVBS2_MAP_SPARSE))) {
        const uint32_t *map = f->mode->bits_per_symbol == 3 ? fast_map_3_packed : fast_map_2_packed;
        const uint32_t *table = (const uint32_t *)((const uint8_t *)map + f->ldxq_bias);
        const unsigned runs = f->pilots ? f->pilot_blocks + 1 : 1;
        for (unsigned r = 0; r < runs; ++r) {
            const unsigned k = f->pilots ? r * PACK_RUN : 0;
            const unsigned left = f->data_symbols - k;
            const unsigned length = f->pilots && left > PACK_RUN ? PACK_RUN : left;
            uint8_t *dst = packed + (90 + k + 36 * r) / 2;
            const uintptr_t at = (uintptr_t)dst;
            uint8_t *block = (uint8_t *)((at - 1) & ~(uintptr_t)15);
            const unsigned vectors = length / 32;
            (f->mode->bits_per_symbol == 3 ? dvbs2_pie_run8_packed : dvbs2_pie_run4_packed)(
                                  index + k / 4, table, block, vectors,
                                  (unsigned)(-at & 15), f->tail);
            memcpy(block + 16 * vectors, f->tail, (at & 15) ? (at & 15) : 16);
            // QPSK's last run has 720 symbols: 22 vectors and 16 symbols (8 bytes).
            for (unsigned j = 32 * vectors; j < length; j += 4) {
                const uint32_t v = map[index[(k + j) / 4]];
                dst[j / 2] = (uint8_t)v;
                dst[j / 2 + 1] = (uint8_t)(v >> 8);
            }
        }
        return 0;
    }
#endif
    return map_packed_runs(f, ws, packed);
}

HOT void dvbs2_fast_pl_packed(const dvbs2_fast *f, uint8_t *packed)
{
    if (f->pie & DVBS2_MAP_QUADS) {
        memcpy(packed, f->packed_header, 22);
        packed[22] = f->packed_header[22] | (packed[22] & 15);
        for (unsigned b = 0; b < f->pilot_blocks; ++b) {
            uint8_t *dst = packed + (90 + 1440*(b+1) + 36*b)/4;
            const uint8_t *src = f->packed_pilots + 9*b;
            dst[0] = (dst[0] & 0xf0) | (src[0] >> 4);
            for (unsigned j = 1; j < 9; ++j) dst[j] = (src[j-1] << 4) | (src[j] >> 4);
            dst[9] = (src[8] << 4) | (dst[9] & 15);
        }
        return;
    }
    memcpy(packed, f->packed_header, 45);
    for (unsigned b = 0; b < f->pilot_blocks; ++b) {
        memcpy(packed + (90 + 1440 * (b + 1) + 36 * b) / 2, f->packed_pilots + 18 * b, 18);
    }
}

void dvbs2_pack(const uint8_t *phases, uint8_t *packed, size_t symbols)
{
    pack_c(phases, packed, (unsigned)symbols);
}

/* Complete frames use the fast path; the original public stages remain the
 * portable reference. Each concurrent encoder owns its context and workspace. */
HOT int dvbs2_encode_frame(dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *ts,
                           const uint8_t *payload, size_t input_bits, uint8_t *output)
{
    if (!output || (ts && (input_bits & 7))) {
        return -1;
    }
    const int rc = ts ? dvbs2_fast_bb_ts(f, ws, ts, payload, input_bits / 8)
                      : dvbs2_fast_bb_gcs(f, ws, payload, input_bits);
    if (rc) {
        return rc;
    }
    dvbs2_fast_bch(f, ws);
    dvbs2_fast_ldpc(f, ws);
    dvbs2_fast_map(f, ws, output);
    dvbs2_fast_pl(f, output);
    return 0;
}

HOT int dvbs2_encode_frame_packed(dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *ts,
                                  const uint8_t *payload, size_t input_bits, uint8_t *packed)
{
    if (!f || !packed || (ts && (input_bits & 7))) {
        return -1;
    }
    const int rc = ts ? dvbs2_fast_bb_ts(f, ws, ts, payload, input_bits / 8)
                      : dvbs2_fast_bb_gcs(f, ws, payload, input_bits);
    if (rc) {
        return rc;
    }
    dvbs2_fast_bch(f, ws);
    dvbs2_fast_ldpc(f, ws);
    dvbs2_fast_map_packed(f, ws, packed);
    dvbs2_fast_pl_packed(f, packed);
    return 0;
}
