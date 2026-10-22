/* Independently authored normal-frame QPSK 2/3 decoder, EN 302 307-1 5.3.
 * Layered NMS: q=L-Rold; Rnew=sign-product * min(127, floor(3*min/4));
 * L=q+Rnew. Only completed, conflict-free tiles become resumable state. */
#include "rx_mc6.h"
#include "mc6_internal.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_memory_utils.h"
#else
#define IRAM_ATTR
#endif

#if defined(ESP_PLATFORM) && defined(RX_MC6_BCH_TABLE_INTERNAL)
#define MC6_BCH_ATTR DRAM_ATTR
#else
#define MC6_BCH_ATTR
#endif
#include "mc6_tables.h"
#include "mc6_bch8.h"

static bool separate(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!a || !b || x > UINTPTR_MAX - na || y > UINTPTR_MAX - nb) return false;
    return x + na <= y || y + nb <= x;
}

bool rx_mc6_pack_llr(const int8_t llr[RX_MC6_N], uint8_t bits[RX_MC6_WORD_BYTES])
{
    if (!separate(llr, RX_MC6_N, bits, RX_MC6_WORD_BYTES)) return false;
    for (unsigned n = 0; n < RX_MC6_WORD_BYTES; ++n) {
        unsigned value = 0;
        for (unsigned j = 0; j < 8; ++j)
            value = (value << 1) | (llr[8U * n + j] < 0);
        bits[n] = (uint8_t)value;
    }
    return true;
}

bool rx_mc6_init(rx_mc6 *d, int16_t belief[RX_MC6_N],
                 rx_mc6_check checks[RX_MC6_CHECKS], const int8_t llr[RX_MC6_N])
{
    if (!d || ((uintptr_t)belief & 1U) || ((uintptr_t)checks & 3U) ||
        !separate(d, sizeof *d, belief, 2U * RX_MC6_N) ||
        !separate(d, sizeof *d, checks, 4U * RX_MC6_CHECKS) ||
        !separate(d, sizeof *d, llr, RX_MC6_N) ||
        !separate(belief, 2U * RX_MC6_N, checks, 4U * RX_MC6_CHECKS) ||
        !separate(belief, 2U * RX_MC6_N, llr, RX_MC6_N) ||
        !separate(checks, 4U * RX_MC6_CHECKS, llr, RX_MC6_N)) return false;
    for (unsigned n = 0; n < RX_MC6_K; ++n) belief[n] = llr[n];
    for (unsigned layer = 0; layer < 60; ++layer)
        for (unsigned column = 0; column < 360; ++column)
            belief[RX_MC6_K + 360U * layer + column] = llr[RX_MC6_K + 60U * column + layer];
    *d = (rx_mc6){.belief = belief, .checks = checks, .initialized = true};
    return true;
}

static bool valid_check(rx_mc6_check s, unsigned degree)
{
    unsigned lo = s.magnitudes & 255U, hi = s.magnitudes >> 8;
    return lo <= hi && hi <= 127 && !(s.metadata & 0xc000U) &&
           (s.metadata >> 10) < degree &&
           (degree == 10 || !(s.metadata & 512U));
}

#if !defined(ESP_PLATFORM) || defined(RX_MC6_SCALAR)
void rx_mc6_leaf(rx_mc6_tile *w)
{
    for (unsigned lane = 0; lane < 8; ++lane) {
        unsigned first = 32767, second = 32767, parity = 0, index = 0;
        for (unsigned e = 0; e < 10; ++e) {
            int q = w->q[e][lane];
            unsigned a = q == -32768 ? 32767U : (unsigned)(q < 0 ? -q : q);
            if (a <= first) { second = first; first = a; index = e; }
            else if (a < second) second = a;
            parity ^= q < 0;
        }
        unsigned m1 = first * 3U / 4U, m2 = second * 3U / 4U;
        if (m1 > 127) m1 = 127;
        if (m2 > 127) m2 = 127;
        unsigned signs = 0;
        for (unsigned e = 0; e < 10; ++e) {
            int q = w->q[e][lane];
            unsigned negative = parity ^ (q < 0);
            int r = (int)(e == index ? m2 : m1);
            if (negative) r = -r;
            int sum = q + r;
            if (sum > 32767) sum = 32767;
            if (sum < -32768) sum = -32768;
            w->q[e][lane] = (int16_t)sum;
            signs |= negative << e;
        }
        w->magnitudes[lane] = (uint16_t)(m1 | (m2 << 8));
        w->metadata[lane] = (uint16_t)(signs | (index << 10));
    }
}

void rx_mc6_block_leaf(rx_mc6_tile *w, unsigned flags)
{
    if (!(flags & 1U)) {
        for (unsigned e = 0; e < 10; ++e)
            for (unsigned lane = 0; lane < 8; ++lane) {
                unsigned meta = w->metadata[lane], mags = w->magnitudes[lane];
                int r = (int)(((meta >> 10) == e ? mags >> 8 : mags) & 255U);
                if (meta & (1U << e)) r = -r;
                int q = w->q[e][lane] - r;
                if (q > 32767) q = 32767;
                if (q < -32768) q = -32768;
                w->q[e][lane] = (int16_t)q;
            }
    }
    if (flags & 2U) w->q[9][0] = 32767;
    rx_mc6_leaf(w);
}
#endif

static unsigned position(unsigned first, unsigned column)
{
    unsigned p = first + column;
    return p < 360 ? p : p - 360;
}

/* Each raw edge is one contiguous span, or two at the 360-value wrap. All
 * PSRAM accesses remain outside the leaf's interrupt-masked ownership window. */
static void gather(int16_t out[8], const int16_t *row, unsigned p, unsigned lanes)
{
    unsigned head = 360U - p;
    if (head > lanes) head = lanes;
    for (unsigned n = 0; n < head; ++n) out[n] = row[p + n];
    for (unsigned n = head; n < lanes; ++n) out[n] = row[n - head];
    for (unsigned n = lanes; n < 8; ++n) out[n] = 32767;
}

static void scatter(int16_t *row, const int16_t *in, unsigned p, unsigned lanes)
{
    unsigned head = 360U - p;
    if (head > lanes) head = lanes;
    for (unsigned n = 0; n < head; ++n) row[p + n] = in[n];
    for (unsigned n = head; n < lanes; ++n) row[n - head] = in[n];
}

static unsigned step(rx_mc6 *d, unsigned budget, bool blocked)
{
    if (!d || !budget) return 0;
    if (!d->initialized || d->failed) return 0;
    if (!d->belief || !d->checks || d->layer >= 60 || d->column >= 360 ||
        d->column % (d->layer ? 8U : 4U) || d->iteration == UINT32_MAX) {
        d->failed = true;
        return 0;
    }
    _Alignas(16) rx_mc6_tile w;
#ifdef ESP_PLATFORM
    if (!esp_ptr_internal(&w) || !esp_ptr_internal((const uint8_t *)&w + sizeof w - 1U)) {
        d->failed = true;
        return 0;
    }
#endif
    for (unsigned n = 0; n < 4; ++n) w.before[n] = w.after[n] = UINT32_C(0x6d6d6d6d);
    unsigned done = 0;
    unsigned geometry_layer = 60;
    uint16_t base[10], start[10];
    while (done < budget && !d->failed) {
        unsigned layer = d->layer, column = d->column, lanes = layer ? 8U : 4U;
        if (!blocked || geometry_layer != layer) {
            for (unsigned e = 0; e < 8; ++e) {
                unsigned token = mc6_graph[layer][e], shift = token & 511U;
                base[e] = (uint16_t)(360U * (token >> 9));
                start[e] = (uint16_t)(shift ? 360U - shift : 0);
            }
            base[8] = (uint16_t)(RX_MC6_K + 360U * layer);
            start[8] = 0;
            base[9] = (uint16_t)(RX_MC6_K + 360U * (layer ? layer - 1U : 59U));
            start[9] = layer ? 0 : 359;
            geometry_layer = layer;
        }
        /* Metadata is read once per tile, never read during the first sweep. */
        for (unsigned lane = 0; lane < lanes; ++lane) {
            rx_mc6_check s = {0, 0};
            if (d->iteration) s = d->checks[360U * layer + column + lane];
            if (!valid_check(s, !layer && !column && !lane ? 9U : 10U)) {
                d->failed = true;
                break;
            }
            w.magnitudes[lane] = s.magnitudes;
            w.metadata[lane] = s.metadata;
        }
        if (d->failed) break;
        if (blocked) {
            for (unsigned lane = lanes; lane < 8; ++lane) w.magnitudes[lane] = w.metadata[lane] = 0;
            for (unsigned e = 0; e < 10; ++e)
                gather(w.q[e], d->belief + base[e], position(start[e], column), lanes);
            unsigned flags = (!d->iteration ? 1U : 0U) | (!layer && !column ? 2U : 0U);
            rx_mc6_block_leaf(&w, flags);
        } else {
            for (unsigned e = 0; e < 10; ++e) {
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    unsigned p = position(start[e], column + lane);
                    unsigned meta = w.metadata[lane], mags = w.magnitudes[lane];
                    int r = (int)(((meta >> 10) == e ? mags >> 8 : mags) & 255U);
                    if (meta & (1U << e)) r = -r;
                    int q = d->belief[base[e] + p] - r;
                    if (q > 32767) q = 32767;
                    if (q < -32768) q = -32768;
                    w.q[e][lane] = (int16_t)q;
                }
                for (unsigned lane = lanes; lane < 8; ++lane) w.q[e][lane] = 32767;
            }
            if (!layer && !column) w.q[9][0] = 32767;
            rx_mc6_leaf(&w);
        }
        for (unsigned n = 0; n < 4; ++n)
            if (w.before[n] != UINT32_C(0x6d6d6d6d) || w.after[n] != UINT32_C(0x6d6d6d6d))
                d->failed = true;
        if (d->failed) break;
        if (!layer && !column) {
            w.metadata[0] &= (uint16_t)~512U;
            /* With full-range external beliefs all nine real magnitudes can
             * tie the fake +32767 edge. Both normalized minima are then 127;
             * choose the last REAL edge, exactly as a degree-9 reduction. */
            if ((w.metadata[0] >> 10) == 9U)
                w.metadata[0] = (uint16_t)((w.metadata[0] & 1023U) | (8U << 10));
        }
        for (unsigned lane = 0; lane < lanes; ++lane)
            if (!valid_check((rx_mc6_check){w.magnitudes[lane], w.metadata[lane]},
                             !layer && !column && !lane ? 9U : 10U)) d->failed = true;
        if (d->failed) break;
        for (unsigned e = 0; e < 10; ++e) {
            if (blocked) {
                unsigned p = position(start[e], column), skip = !layer && !column && e == 9 ? 1U : 0U;
                if (skip) {
                    if (++p == 360) p = 0;
                }
                scatter(d->belief + base[e], w.q[e] + skip, p, lanes - skip);
                continue;
            }
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (!layer && !column && !lane && e == 9) continue;
                unsigned p = position(start[e], column + lane);
                d->belief[base[e] + p] = w.q[e][lane];
            }
        }
        for (unsigned lane = 0; lane < lanes; ++lane)
            d->checks[360U * layer + column + lane] = (rx_mc6_check){w.magnitudes[lane], w.metadata[lane]};
        ++done;
        d->column = (uint16_t)(column + lanes);
        if (d->column == 360) {
            d->column = 0;
            if (++d->layer == 60) {
                d->layer = 0;
                if (++d->iteration == UINT32_MAX) break;
            }
        }
    }
    return done;
}

unsigned rx_mc6_step(rx_mc6 *d, unsigned budget)
{
    return step(d, budget, false);
}

unsigned rx_mc6_step_blocked(rx_mc6 *d, unsigned budget)
{
    return step(d, budget, true);
}

bool rx_mc6_pack(const rx_mc6 *d, uint8_t bits[RX_MC6_WORD_BYTES])
{
    if (!d || !d->initialized || d->failed || !bits ||
        !separate(bits, RX_MC6_WORD_BYTES, d, sizeof *d) ||
        !separate(bits, RX_MC6_WORD_BYTES, d->belief, 2U * RX_MC6_N) ||
        !separate(bits, RX_MC6_WORD_BYTES, d->checks, 4U * RX_MC6_CHECKS)) return false;
    memset(bits, 0, RX_MC6_WORD_BYTES);
    for (unsigned n = 0; n < RX_MC6_N; ++n) {
        unsigned at = n < RX_MC6_K ? n : RX_MC6_K + 360U * ((n - RX_MC6_K) % 60U) + (n - RX_MC6_K) / 60U;
        if (d->belief[at] < 0) bits[n / 8] |= (uint8_t)(128U >> (n % 8U));
    }
    return true;
}

/* A systematic QC block is exactly 45 bytes. Extract 32 MSB-first bits,
 * wrapping inside that block, never into the next one or a guard byte. */
static uint32_t circular32(const uint8_t *row, unsigned position)
{
    unsigned byte = position / 8U, shift = position % 8U;
    uint32_t value = 0;
    for (unsigned n = 0; n < 4; ++n) {
        value = (value << 8) | row[byte];
        if (++byte == 45) byte = 0;
    }
    return shift ? (value << shift) | (row[byte] >> (8U - shift)) : value;
}

static unsigned ones32(uint32_t value)
{
    value -= (value >> 1) & UINT32_C(0x55555555);
    value = (value & UINT32_C(0x33333333)) + ((value >> 2) & UINT32_C(0x33333333));
    value = (value + (value >> 4)) & UINT32_C(0x0f0f0f0f);
    return (value * UINT32_C(0x01010101)) >> 24;
}

/* MSB-first natural parity window; the last group has only four live bits.
 * Load the following byte only when a live bit crosses its boundary. */
static uint8_t window(const uint8_t *p, unsigned k, unsigned live)
{
    unsigned shift = k % 8U, value = (unsigned)p[k / 8U] << shift;
    if (shift + live > 8) value |= p[k / 8U + 1U] >> (8U - shift);
    return (uint8_t)(value & (255U << (8U - live)));
}

/* At each stage exchange the off-diagonal bit groups of size d. For example
 * d=4 exchanges the low nibble of row j with the high nibble of row j+4.
 * The successive 4/2/1 exchanges map input row j bit 7-l to row l bit 7-j. */
static void transpose8(uint8_t x[8])
{
    for (unsigned d = 4; d; d /= 2) {
        unsigned mask = d == 4 ? 15U : d == 2 ? 51U : 85U;
        for (unsigned group = 0; group < 8; group += 2U * d)
            for (unsigned n = 0; n < d; ++n) {
                unsigned a = group + n, b = a + d;
                unsigned exchange = (x[a] ^ (x[b] >> d)) & mask;
                x[a] ^= (uint8_t)exchange;
                x[b] ^= (uint8_t)(exchange << d);
            }
    }
}

static unsigned syndrome(const uint8_t *bits)
{
    struct {
        uint32_t before, parity[8];
        uint8_t x[8];
        uint32_t after;
    } w = {.before = UINT32_C(0x6d6d6d6d), .after = UINT32_C(0x6d6d6d6d)};
    const uint8_t *p = bits + 5400;
    unsigned bad = 0;
    for (unsigned layer = 0; layer < 60; layer += 8) {
        unsigned live = layer == 56 ? 4U : 8U;
        for (unsigned first = 0; first < 360; first += 32) {
            unsigned count = first == 352 ? 8U : 32U;
            for (unsigned l = 0; l < live; ++l) w.parity[l] = 0;
            for (unsigned group = 0; group < count; group += 8) {
                for (unsigned j = 0; j < 8; ++j) {
                    unsigned k = 60U * (first + group + j) + layer;
                    unsigned previous = k ? window(p, k - 1U, live) : p[0] >> 1;
                    w.x[j] = (uint8_t)(window(p, k, live) ^ previous);
                }
                transpose8(w.x);
                for (unsigned l = 0; l < live; ++l)
                    w.parity[l] = (w.parity[l] << 8) | w.x[l];
            }
            for (unsigned l = 0; l < live; ++l) {
                uint32_t word = count == 8 ? w.parity[l] << 24 : w.parity[l];
                for (unsigned e = 0; e < 8; ++e) {
                    unsigned token = mc6_graph[layer + l][e], shift = token & 511U;
                    unsigned position = first + 360U - shift;
                    if (position >= 360) position -= 360;
                    word ^= circular32(bits + 45U * (token >> 9), position);
                }
                if (count == 8) word &= UINT32_C(0xff000000);
                bad += ones32(word);
            }
        }
    }
    if (w.before != UINT32_C(0x6d6d6d6d) || w.after != UINT32_C(0x6d6d6d6d)) return UINT32_MAX;
    return bad;
}

static uint8_t descramble(uint8_t b, unsigned *state)
{
    unsigned mask = ((*state >> 6) ^ (*state >> 7)) & 255U;
    *state = ((*state << 8) | mask) & 0x7fffU;
    return b ^ (uint8_t)mask;
}

static bool header(const uint8_t *bits, uint16_t *bytes)
{
    uint8_t h[10];
    unsigned prbs = 0xa9U, crc = 0;
    for (unsigned n = 0; n < 10; ++n) h[n] = descramble(bits[n], &prbs);
    for (unsigned n = 0; n < 72; ++n) {
        unsigned feedback = (crc >> 7) ^ ((h[n / 8] >> (7U - n % 8U)) & 1U);
        crc = (crc << 1) & 255U;
        if (feedback) crc ^= 0xd5U;
    }
    unsigned dfl = ((unsigned)h[4] << 8) | h[5];
    unsigned upl = ((unsigned)h[2] << 8) | h[3];
    unsigned syncd = ((unsigned)h[7] << 8) | h[8];
    if (crc != h[9] || (h[0] & 0x3cU) != 0x30U || h[1] ||
        (h[0] & 3U) == 3 || !dfl || dfl > RX_MC6_KBCH - 80 || (dfl & 7U)) return false;
    unsigned type = h[0] >> 6;
    if (type == 1) {
        if (upl || syncd || h[6]) return false;
    } else if (type == 3) {
        if (upl != 1504 || h[6] != 0x47 || syncd >= upl || syncd >= dfl) return false;
    } else return false;
    *bytes = (uint16_t)(dfl / 8U);
    return true;
}

bool rx_mc6_check_h(const uint8_t bits[RX_MC6_WORD_BYTES], uint32_t *unsatisfied)
{
    if (!separate(bits, RX_MC6_WORD_BYTES, unsatisfied, sizeof *unsatisfied)) return false;
    *unsatisfied = syndrome(bits);
    return *unsatisfied == 0;
}

bool rx_mc6_check_bch(const uint8_t bits[RX_MC6_WORD_BYTES], rx_mc6_bch kind)
{
    if (!bits || (kind != RX_MC6_BCH_NIBBLE && kind != RX_MC6_BCH_BYTE)) return false;
    uint32_t bch[5] = {0};
    if (kind == RX_MC6_BCH_BYTE) {
        for (unsigned n = 0; n < RX_MC6_K / 8; ++n) {
            unsigned feedback = (bch[4] >> 24) ^ bits[n];
            for (unsigned k = 5; k-- > 0;)
                bch[k] = ((bch[k] << 8) | (k ? bch[k - 1] >> 24 : 0)) ^ mc6_bch8[feedback][k];
        }
    } else {
        for (unsigned n = 0; n < RX_MC6_K / 8; ++n) {
            for (unsigned half = 0; half < 2; ++half) {
                unsigned nibble = (bits[n] >> (half ? 0 : 4)) & 15U;
                unsigned feedback = (bch[4] >> 28) ^ nibble;
                for (unsigned k = 5; k-- > 0;) {
                    bch[k] = (bch[k] << 4) | (k ? bch[k - 1] >> 28 : 0);
                    bch[k] ^= mc6_bch4[feedback][k];
                }
            }
        }
    }
    return !(bch[0] | bch[1] | bch[2] | bch[3] | bch[4]);
}

bool rx_mc6_check_header(const uint8_t bits[RX_MC6_WORD_BYTES], uint16_t *bytes)
{
    if (!separate(bits, RX_MC6_WORD_BYTES, bytes, sizeof *bytes)) return false;
    return header(bits, bytes);
}

static bool check_word(const uint8_t *bits, rx_mc6_result *r, rx_mc6_bch kind)
{
    *r = (rx_mc6_result){0};
    r->unsatisfied = syndrome(bits);
    r->bch_ok = rx_mc6_check_bch(bits, kind);
    r->header_ok = header(bits, &r->payload_bytes);
    return !r->unsatisfied && r->bch_ok && r->header_ok;
}

bool rx_mc6_check_word(const uint8_t bits[RX_MC6_WORD_BYTES], rx_mc6_result *r)
{
    if (!separate(bits, RX_MC6_WORD_BYTES, r, sizeof *r)) return false;
    return check_word(bits, r, RX_MC6_BCH_NIBBLE);
}

static void descramble_data(const uint8_t *bits, uint8_t *payload, size_t bytes)
{
    unsigned prbs = 0xa9U;
    for (unsigned n = 0; n < 10; ++n) (void)descramble(bits[n], &prbs);
    for (size_t n = 0; n < bytes; ++n) payload[n] = descramble(bits[10U + n], &prbs);
}

#ifdef RX_MC6_TEST_HOOKS
bool rx_mc6_descramble_data(const uint8_t bits[RX_MC6_WORD_BYTES], uint8_t *payload, size_t bytes)
{
    if (bytes > RX_MC6_PAYLOAD_BYTES || !separate(bits, RX_MC6_WORD_BYTES, payload, bytes)) return false;
    descramble_data(bits, payload, bytes);
    return true;
}
#endif

bool rx_mc6_finish(const uint8_t bits[RX_MC6_WORD_BYTES], uint8_t *payload,
                   size_t capacity, size_t *bytes, rx_mc6_result *r, rx_mc6_bch kind)
{
    if ((kind != RX_MC6_BCH_NIBBLE && kind != RX_MC6_BCH_BYTE) ||
        !separate(bits, RX_MC6_WORD_BYTES, payload, capacity) ||
        !separate(bits, RX_MC6_WORD_BYTES, bytes, sizeof *bytes) ||
        !separate(bits, RX_MC6_WORD_BYTES, r, sizeof *r) ||
        !separate(payload, capacity, bytes, sizeof *bytes) ||
        !separate(payload, capacity, r, sizeof *r) ||
        !separate(bytes, sizeof *bytes, r, sizeof *r)) return false;
    *bytes = 0;
    rx_mc6_result result;
    bool valid = check_word(bits, &result, kind);
    *r = result;
    if (!valid || capacity < result.payload_bytes) return false;
    descramble_data(bits, payload, result.payload_bytes);
    *bytes = result.payload_bytes;
    return true;
}

bool rx_mc6_extract(const uint8_t bits[RX_MC6_WORD_BYTES], uint8_t *payload,
                    size_t capacity, size_t *bytes)
{
    if (!bytes || !payload || !separate(bits, RX_MC6_WORD_BYTES, bytes, sizeof *bytes) ||
        !separate(bits, RX_MC6_WORD_BYTES, payload, capacity) ||
        !separate(bytes, sizeof *bytes, payload, capacity)) return false;
    rx_mc6_result r;
    if (!rx_mc6_check_word(bits, &r) || capacity < r.payload_bytes) return false;
    descramble_data(bits, payload, r.payload_bytes);
    *bytes = r.payload_bytes;
    return true;
}
