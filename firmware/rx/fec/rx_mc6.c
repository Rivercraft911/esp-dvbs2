/* Independently authored normal-frame QPSK 2/3 decoder, EN 302 307-1 5.3.
 * Layered NMS: q=L-Rold; Rnew=sign-product * min(127, floor(3*min/4));
 * L=q+Rnew. Only completed, conflict-free tiles become resumable state. */
#include "rx_mc6.h"
#include "mc6_internal.h"
#include "mc6_tables.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_memory_utils.h"
#else
#define IRAM_ATTR
#endif

static bool separate(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!a || !b || x > UINTPTR_MAX - na || y > UINTPTR_MAX - nb) return false;
    return x + na <= y || y + nb <= x;
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
#endif

static unsigned position(unsigned first, unsigned column)
{
    unsigned p = first + column;
    return p < 360 ? p : p - 360;
}

unsigned rx_mc6_step(rx_mc6 *d, unsigned budget)
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
    while (done < budget && !d->failed) {
        unsigned layer = d->layer, column = d->column, lanes = layer ? 8U : 4U;
        uint16_t base[10], start[10];
        for (unsigned e = 0; e < 8; ++e) {
            unsigned token = mc6_graph[layer][e], shift = token & 511U;
            base[e] = (uint16_t)(360U * (token >> 9));
            start[e] = (uint16_t)(shift ? 360U - shift : 0);
        }
        base[8] = (uint16_t)(RX_MC6_K + 360U * layer);
        start[8] = 0;
        base[9] = (uint16_t)(RX_MC6_K + 360U * (layer ? layer - 1U : 59U));
        start[9] = layer ? 0 : 359;
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
        for (unsigned e = 0; e < 10; ++e)
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (!layer && !column && !lane && e == 9) continue;
                unsigned p = position(start[e], column + lane);
                d->belief[base[e] + p] = w.q[e][lane];
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

static unsigned bit(const uint8_t *p, unsigned n)
{
    return (p[n / 8] >> (7U - n % 8U)) & 1U;
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

bool rx_mc6_check_word(const uint8_t bits[RX_MC6_WORD_BYTES], rx_mc6_result *r)
{
    if (!r || !separate(bits, RX_MC6_WORD_BYTES, r, sizeof *r)) return false;
    *r = (rx_mc6_result){0};
    for (unsigned layer = 0; layer < 60; ++layer)
        for (unsigned column = 0; column < 360; ++column) {
            unsigned k = 60U * column + layer;
            unsigned value = bit(bits, RX_MC6_K + k);
            if (k) value ^= bit(bits, RX_MC6_K + k - 1U);
            for (unsigned e = 0; e < 8; ++e) {
                unsigned token = mc6_graph[layer][e], shift = token & 511U;
                unsigned p = column + 360U - shift;
                if (p >= 360) p -= 360;
                value ^= bit(bits, 360U * (token >> 9) + p);
            }
            r->unsatisfied += value;
        }
    uint32_t bch[5] = {0};
    for (unsigned n = 0; n < RX_MC6_K / 8; ++n)
        for (unsigned half = 0; half < 2; ++half) {
            unsigned nibble = (bits[n] >> (half ? 0 : 4)) & 15U;
            unsigned feedback = (bch[4] >> 28) ^ nibble;
            for (unsigned k = 5; k-- > 0;) {
                bch[k] = (bch[k] << 4) | (k ? bch[k - 1] >> 28 : 0);
                bch[k] ^= mc6_bch4[feedback][k];
            }
        }
    r->bch_ok = !(bch[0] | bch[1] | bch[2] | bch[3] | bch[4]);
    r->header_ok = header(bits, &r->payload_bytes);
    return !r->unsatisfied && r->bch_ok && r->header_ok;
}

bool rx_mc6_extract(const uint8_t bits[RX_MC6_WORD_BYTES], uint8_t *payload,
                    size_t capacity, size_t *bytes)
{
    if (!bytes || !payload || !separate(bits, RX_MC6_WORD_BYTES, bytes, sizeof *bytes) ||
        !separate(bits, RX_MC6_WORD_BYTES, payload, capacity) ||
        !separate(bytes, sizeof *bytes, payload, capacity)) return false;
    rx_mc6_result r;
    if (!rx_mc6_check_word(bits, &r) || capacity < r.payload_bytes) return false;
    unsigned prbs = 0xa9U;
    for (unsigned n = 0; n < 10; ++n) (void)descramble(bits[n], &prbs);
    for (unsigned n = 0; n < r.payload_bytes; ++n) payload[n] = descramble(bits[10U + n], &prbs);
    *bytes = r.payload_bytes;
    return true;
}
