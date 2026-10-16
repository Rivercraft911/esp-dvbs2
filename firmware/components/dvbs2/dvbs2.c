/* Independent normal-frame encoder. ETSI EN 302 307-1 V1.4.1 clauses 5.1-5.5.
 * Portable C99, no platform headers. DVBS2_IRAM places timed stages in ESP32
 * instruction RAM; constant tables stay in flash and use the normal data cache. */
#include "dvbs2.h"
#include "private.h"

#include <string.h>

#ifdef DVBS2_IRAM
#define HOT __attribute__((section(".iram1"), noinline))
#else
#define HOT
#endif

#include "tables.h"

const dvbs2_mode dvbs2_modes[] = {
    {"1/2", 32208, 32400, 4, 2, 90, 12},
    {"3/5", 38688, 38880, 5, 2, 72, 12},
    {"2/3", 43040, 43200, 6, 2, 60, 10},
    {"3/4", 48408, 48600, 7, 2, 45, 12},
    {"4/5", 51648, 51840, 8, 2, 36, 12},
    {"5/6", 53840, 54000, 9, 2, 30, 10},
    {"8/9", 57472, 57600, 10, 2, 20, 8},
    {"9/10", 58192, 58320, 11, 2, 18, 8},
    {"3/5", 38688, 38880, 12, 3, 72, 12},
    {"2/3", 43040, 43200, 13, 3, 60, 10},
    {"3/4", 48408, 48600, 14, 3, 45, 12},
    {"5/6", 53840, 54000, 15, 3, 30, 10},
    {"8/9", 57472, 57600, 16, 3, 20, 8},
    {"9/10", 58192, 58320, 17, 3, 18, 8},
};
const size_t dvbs2_mode_count = sizeof dvbs2_modes / sizeof dvbs2_modes[0];

const dvbs2_mode *dvbs2_find_mode(unsigned modcod)
{
    if (modcod < 4 || modcod > 17) {
        return NULL;
    }
    return &dvbs2_modes[modcod - 4];
}

static inline int valid_mode(const dvbs2_mode *mode)
{
    if (!mode) {
        return 0;
    }
    const dvbs2_mode *known = dvbs2_find_mode(mode->modcod);
    return known && mode->kbch == known->kbch && mode->nbch == known->nbch &&
           mode->bits_per_symbol == known->bits_per_symbol &&
           mode->q == known->q && mode->t == known->t;
}

size_t dvbs2_frame_symbols(const dvbs2_mode *mode, int pilots)
{
    if (!valid_mode(mode)) {
        return 0;
    }
    size_t slots = 64800 / (90 * mode->bits_per_symbol);
    return 90 * (slots + 1) + (pilots ? 36 * ((slots - 1) / 16) : 0);
}

static inline uint8_t crc8_byte(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0xd5 : 0));
    }
    return crc;
}

static inline void bb_header(uint8_t *out, uint8_t matype, unsigned dfl,
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
        crc = crc8_byte(crc, out[k]);
    }
    out[9] = crc;
}

HOT int dvbs2_bb_gcs(dvbs2_workspace *ws, const dvbs2_mode *mode,
                     const uint8_t *payload, size_t dfl_bits)
{
    if (!ws || !valid_mode(mode) || dfl_bits > (size_t)mode->kbch - 80 ||
        (!payload && dfl_bits)) {
        return -1;
    }
    memset(ws->bits, 0, mode->kbch / 8);
    bb_header(ws->bits, 0x70, (unsigned)dfl_bits, 0, 0, 0);
    if (dfl_bits) {
        memcpy(ws->bits + 10, payload, (dfl_bits + 7) / 8);
        if (dfl_bits & 7) {
            ws->bits[10 + dfl_bits / 8] &= (uint8_t)(0xff << (8 - (dfl_bits & 7)));
        }
    }
    return 0;
}

HOT int dvbs2_bb_ts(dvbs2_workspace *ws, const dvbs2_mode *mode,
                    dvbs2_ts_state *state, const uint8_t *input, size_t input_bytes)
{
    if (!ws || !valid_mode(mode) || !state || !input ||
        state->packet_pos >= 188 || input_bytes != ((size_t)mode->kbch - 80) / 8) {
        return -1;
    }
    /* Validate before changing either state or output. */
    size_t next_sync = state->packet_pos ? 188 - state->packet_pos : 0;
    for (size_t k = next_sync; k < input_bytes; k += 188) {
        if (input[k] != 0x47) {
            return -1;
        }
    }
    unsigned syncd = ((188 - state->packet_pos) % 188) * 8;
    bb_header(ws->bits, 0xf0, (unsigned)(input_bytes * 8), 1504, 0x47, syncd);
    for (size_t k = 0; k < input_bytes; ++k) {
        if (state->packet_pos == 0) {
            ws->bits[10 + k] = state->previous_crc;
            state->crc = 0;
        } else {
            ws->bits[10 + k] = input[k];
            state->crc = crc8_byte(state->crc, input[k]);
        }
        if (++state->packet_pos == 188) {
            state->packet_pos = 0;
            state->previous_crc = state->crc;
        }
    }
    return 0;
}

HOT void dvbs2_bb_scramble(dvbs2_workspace *ws, const dvbs2_mode *mode)
{
    for (unsigned k = 0; k < mode->kbch / 8; ++k) {
        ws->bits[k] ^= bb_prbs[k];
    }
}

HOT void dvbs2_bch(dvbs2_workspace *ws, const dvbs2_mode *mode)
{
    const uint32_t (*table)[6] = mode->t == 12 ? bch_12 :
                                 mode->t == 10 ? bch_10 : bch_8;
    unsigned words = (mode->nbch - mode->kbch) / 32;
    uint32_t reg[6] = {0}; /* Most significant word first. */
    for (unsigned k = 0; k < mode->kbch / 8; ++k) {
        unsigned index = (reg[0] >> 24) ^ ws->bits[k];
        for (unsigned w = 0; w < words; ++w) {
            uint32_t next = w + 1 < words ? reg[w + 1] : 0;
            reg[w] = (reg[w] << 8) ^ (next >> 24) ^ table[index][w];
        }
    }
    for (unsigned w = 0; w < words; ++w) {
        for (unsigned b = 0; b < 4; ++b) {
            ws->bits[mode->kbch / 8 + 4 * w + b] = (uint8_t)(reg[w] >> (24 - 8 * b));
        }
    }
}

static inline const uint16_t *ldpc_rows(unsigned modcod)
{
    static const uint16_t *const tables[] = {
        ldpc_0, ldpc_1, ldpc_2, ldpc_3, ldpc_4, ldpc_5, ldpc_6, ldpc_7,
        ldpc_1, ldpc_2, ldpc_3, ldpc_5, ldpc_6, ldpc_7,
    };
    return tables[modcod - 4];
}

HOT void dvbs2_ldpc(dvbs2_workspace *ws, const dvbs2_mode *mode)
{
    unsigned q = mode->q;
    memset(ws->parity, 0, q * sizeof ws->parity[0]);
    const uint16_t *row = ldpc_rows(mode->modcod);
    for (unsigned group = 0; group < mode->nbch / 360; ++group) {
        /* Repeat the 360-bit input twice so each cyclic rotation is a plain
         * pair of shifted 32-bit loads. Bit t is held in bit t%32. */
        uint32_t u[24] = {0};
        const uint8_t *input = ws->bits + 45 * group;
        for (unsigned k = 0; k < 45; ++k) {
            u[k / 4] |= (uint32_t)reverse_byte[input[k]] << (8 * (k % 4));
        }
        uint32_t tail = u[11];
        for (unsigned k = 0; k < 11; ++k) {
            u[11 + k] |= u[k] << 8;
            u[12 + k] = u[k] >> 24;
        }
        u[22] |= tail << 8;
        unsigned degree = *row++;
        for (unsigned edge = 0; edge < degree; ++edge) {
            unsigned address = *row++;
            uint32_t *dest = ws->parity[address >> 9];
            unsigned column = address & 511;
            unsigned start = column ? 360 - column : 0;
            unsigned word = start / 32, shift = start % 32;
            if (shift == 0) {
                for (unsigned w = 0; w < 12; ++w) {
                    dest[w] ^= u[word + w];
                }
            } else {
                for (unsigned w = 0; w < 12; ++w) {
                    dest[w] ^= (u[word + w] >> shift) |
                               (u[word + w + 1] << (32 - shift));
                }
            }
        }
    }
    /* Prefix XOR down all q rows, processing 32 columns at once. */
    for (unsigned r = 1; r < q; ++r) {
        for (unsigned w = 0; w < 12; ++w) {
            ws->parity[r][w] ^= ws->parity[r - 1][w];
        }
    }
    /* Each column carries the parity of every preceding column. */
    uint32_t carries[12];
    uint32_t carry = 0;
    for (unsigned w = 0; w < 12; ++w) {
        uint32_t prefix = ws->parity[q - 1][w];
        prefix ^= prefix << 1;
        prefix ^= prefix << 2;
        prefix ^= prefix << 4;
        prefix ^= prefix << 8;
        prefix ^= prefix << 16;
        if (carry) {
            prefix = ~prefix;
        }
        carries[w] = (prefix << 1) | carry;
        carry = prefix >> 31;
    }
    for (unsigned r = 0; r < q; ++r) {
        for (unsigned w = 0; w < 12; ++w) {
            ws->parity[r][w] ^= carries[w];
        }
    }
    /* Transpose the q x 360 bit matrix into the natural parity order. */
    uint8_t *out = ws->bits + mode->nbch / 8;
    unsigned count = 0;
    uint8_t byte = 0;
    for (unsigned t = 0; t < 360; ++t) {
        for (unsigned r = 0; r < q; ++r) {
            byte = (uint8_t)((byte << 1) | ((ws->parity[r][t / 32] >> (t % 32)) & 1));
            if (++count == 8) {
                *out++ = byte;
                count = 0;
            }
        }
    }
}

static inline unsigned bit_at(const uint8_t *bits, unsigned index)
{
    return (bits[index / 8] >> (7 - index % 8)) & 1;
}

HOT void dvbs2_map(const dvbs2_workspace *ws, const dvbs2_mode *mode,
                   uint8_t *data_phases)
{
    if (mode->bits_per_symbol == 2) {
        static const uint8_t phases[] = {1, 7, 3, 5}; /* Figure 9. */
        for (unsigned k = 0; k < 32400; ++k) {
            unsigned pair = (ws->bits[k / 4] >> (6 - 2 * (k % 4))) & 3;
            data_phases[k] = phases[pair];
        }
    } else {
        static const uint8_t phases[] = {1, 0, 4, 5, 2, 7, 3, 6}; /* Figure 10. */
        for (unsigned k = 0; k < 21600; ++k) {
            unsigned a = bit_at(ws->bits, k);
            unsigned b = bit_at(ws->bits, k + 21600);
            unsigned c = bit_at(ws->bits, k + 43200);
            /* 5.3.3: rate 3/5 reads the columns in reverse order. */
            unsigned label = mode->modcod == 12 ? (c << 2) | (b << 1) | a :
                                                  (a << 2) | (b << 1) | c;
            data_phases[k] = phases[label];
        }
    }
}

HOT void dvbs2_plframe(const dvbs2_mode *mode, int pilots, uint8_t *output)
{
    size_t data_symbols = 64800 / mode->bits_per_symbol;
    size_t blocks = pilots ? (data_symbols / 90 - 1) / 16 : 0;
    if (blocks) {
        /* Expanding from the last block preserves mapped data in this buffer. */
        for (size_t block = blocks + 1; block-- > 0;) {
            size_t start = block * 1440;
            size_t length = data_symbols - start;
            if (length > 1440) {
                length = 1440;
            }
            memmove(output + 90 + start + 36 * block, output + 90 + start, length);
            if (block < blocks) {
                memset(output + 90 + start + 36 * block + length, 1, 36);
            }
        }
    }
    size_t body_symbols = data_symbols + 36 * blocks;
    for (size_t k = 0; k < body_symbols; ++k) {
        unsigned rotation = (pl_rotations[k / 4] >> (2 * (k % 4))) & 3;
        output[90 + k] = (uint8_t)((output[90 + k] + 2 * rotation) & 7);
    }
    dvbs2_plheader(mode->modcod, pilots, output);
}

HOT void dvbs2_plheader(unsigned modcod, int pilots, uint8_t *output)
{
    /* 5.5.2: SOF and scrambled (64,7) PLS code, pi/2-BPSK mapped. */
    static const uint32_t generators[6] = {
        0x55555555u, 0x33333333u, 0x0f0f0f0fu,
        0x00ff00ffu, 0x0000ffffu, 0xffffffffu,
    };
    uint32_t y = 0;
    for (unsigned k = 0; k < 5; ++k) {
        if ((modcod >> (4 - k)) & 1) {
            y ^= generators[k];
        }
    }
    const uint64_t scramble = UINT64_C(0x719d83c953422dfa);
    for (unsigned k = 0; k < 90; ++k) {
        unsigned bit;
        if (k < 26) {
            bit = (0x18d2e82u >> (25 - k)) & 1;
        } else {
            unsigned p = k - 26;
            bit = ((y >> (31 - p / 2)) ^ (scramble >> (63 - p))) & 1;
            if (pilots && (p & 1)) {
                bit ^= 1;
            }
        }
        output[k] = (uint8_t)(((k & 1) ? 3 : 1) + (bit ? 4 : 0));
    }
}

const uint32_t (*dvbs2_bch_bytes(unsigned t))[6]
{
    return t == 12 ? bch_12 : t == 10 ? bch_10 : bch_8;
}

const uint16_t *dvbs2_ldpc_table(unsigned modcod)
{
    return ldpc_rows(modcod);
}

const uint8_t *dvbs2_prbs_table(void)
{
    return bb_prbs;
}

unsigned dvbs2_pl_rotation(size_t k)
{
    return (pl_rotations[k / 4] >> (2 * (k % 4))) & 3;
}
