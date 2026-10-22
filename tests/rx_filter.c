/* Independent whole-stream integer oracle. No expected payload in the filter. */
#include "rx_filter.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned controls;
static uint32_t rng = 20261022;
#ifdef RX_FILTER_MOCK_PIE
static int fail_after = -1;
/* Exercise the production staging branch on the host. This is explicitly
 * not an instruction emulator or native context proof. */
bool rx_filter_unpack_s8(const int8_t *src, int8_t *i, int8_t *q, unsigned groups)
{
    assert(!((uintptr_t)src & 31) && !((uintptr_t)i & 15) && !((uintptr_t)q & 15));
    for (unsigned n = 0; n < 16*groups; ++n) { i[n] = src[2*n]; q[n] = src[2*n+1]; }
    return true;
}
bool rx_filter_dot_s8(const int8_t *i, const int8_t *q, const int8_t *c, int32_t sums[2])
{
    if (fail_after == 0) return false;
    if (fail_after > 0) --fail_after;
    assert(!((uintptr_t)c & 15) && ((uintptr_t)i & 15) == ((uintptr_t)q & 15));
    /* Read the exact enclosing vectors that the leaf can read. */
    unsigned offset = (uintptr_t)i & 15;
    int8_t pi[48], pq[48];
    size_t bytes = offset ? 48 : 32;
    memcpy(pi, (const void *)((uintptr_t)i & ~(uintptr_t)15), bytes);
    memcpy(pq, (const void *)((uintptr_t)q & ~(uintptr_t)15), bytes);
    sums[0] = sums[1] = 0;
    for (unsigned k = 0; k < 32; ++k) {
        sums[0] += (int32_t)pi[offset+k]*c[k];
        sums[1] += (int32_t)pq[offset+k]*c[k];
    }
    return true;
}
#endif
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static int16_t rounded(int64_t x, unsigned shift)
{
    int64_t unit = INT64_C(1) << shift;
    int64_t quotient = x/unit, rem = x%unit;
    if (rem < 0) { --quotient; rem += unit; }
    quotient += rem >= unit/2;
    if (quotient < -32768) return -32768;
    if (quotient > 32767) return 32767;
    return (int16_t)quotient;
}

static void stream(unsigned alignment, unsigned phase, uint64_t step, unsigned pattern)
{
    enum { PAIRS = 2051 };
    _Alignas(16) uint8_t storage[2*PAIRS+64];
    memset(storage, 0x6d, sizeof storage);
    int8_t *input = (int8_t *)storage+alignment;
    for (unsigned k = 0; k < 2*PAIRS; ++k)
        input[k] = pattern == 0 ? -128 : pattern == 1 ? 127 : (int8_t)random32();
    uint8_t immutable[sizeof storage]; memcpy(immutable, storage, sizeof storage);
    struct { _Alignas(16) uint8_t before[16]; rx_filter s; uint8_t after[16]; } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    rx_filter *s = &guarded.s;
    uint32_t fraction = phase == 64 ? UINT32_MAX : phase << 26;
    if (phase >= 65) {
        unsigned b = (phase-65)/3;
        int neighbor = (int)((phase-65)%3)-1;
        fraction = (b << 26) + (UINT32_C(1)<<25) + neighbor;
    }
    assert(rx_filter_init(s, pattern == 3 ? 97 : 0, fraction, step));
    uint64_t origin = s->origin;
    uint32_t frac = fraction;
    unsigned consumed = 0, emitted = 0, calls = 0;
    bool saw_pending = false;
    for (;;) {
        unsigned n = PAIRS-consumed;
        unsigned chunk = calls % 5 == 0 ? 512 : 1+random32()%512;
        if (n > chunk) n = chunk;
        unsigned capacity = calls % 3 == 0 ? 1 : 1+random32()%64;
        struct { int16_t pre[8], value[64][2], post[8]; } p, d;
        memset(&p, 0x31, sizeof p); memset(&d, 0x57, sizeof d);
        uint64_t mask = UINT64_C(0xa55aa55aa55aa55a);
        if (capacity < 64) mask &= (UINT64_C(1)<<capacity)-1;
        rx_filter_progress progress = {999, 999};
        if (pattern == 3 && calls % 7 == 0) {
            step = calls % 14 == 0 ? RX_FILTER_STEP_MIN : RX_FILTER_STEP_MAX;
            assert(rx_filter_set_step(s, step));
        }
        assert(rx_filter_push(s, input+2*consumed, n, p.value, d.value,
                              capacity, mask, &progress));
        assert(progress.consumed <= n && progress.written <= capacity);
        if (n && !progress.consumed && progress.written) saw_pending = true;
        consumed += (unsigned)progress.consumed;
        for (unsigned j = 0; j < progress.written; ++j) {
            uint64_t nearest = ((uint64_t)frac + (UINT64_C(1)<<25))/ (UINT64_C(1)<<26);
            uint64_t at = origin+nearest/64;
            unsigned bank = (unsigned)(nearest%64);
            assert(at+32 <= consumed);
            for (unsigned axis = 0; axis < 2; ++axis) {
                int64_t sum = 0, derivative = 0;
                for (unsigned k = 0; k < 32; ++k) {
                    int sample = input[2*(at+k)+axis];
                    sum += sample*rx_filter_pulse[bank][k];
                    if ((mask>>j)&1) derivative += sample*rx_filter_derivative[bank][k];
                }
                assert(p.value[j][axis] == rounded(sum, 1));
                assert(d.value[j][axis] == rounded(derivative, 9));
            }
            uint64_t advance = (uint64_t)frac+step;
            origin += advance>>32;
            frac = (uint32_t)advance;
            ++emitted;
        }
        for (unsigned j = (unsigned)progress.written; j < 64; ++j) {
            assert(p.value[j][0] == 0x3131 && p.value[j][1] == 0x3131);
            assert(d.value[j][0] == 0x5757 && d.value[j][1] == 0x5757);
        }
        for (unsigned j = 0; j < 8; ++j) {
            assert(p.pre[j] == 0x3131 && p.post[j] == 0x3131);
            assert(d.pre[j] == 0x5757 && d.post[j] == 0x5757);
        }
        for (unsigned j = 0; j < 16; ++j)
            assert(guarded.before[j] == 0xa5 && guarded.after[j] == 0xa5);
        assert(s->origin == origin && s->fraction == frac);
        assert(++calls < 10000);
        if (consumed == PAIRS && !progress.written) break;
        assert(progress.consumed || progress.written);
    }
    assert(saw_pending && emitted > 450);
    assert(!memcmp(immutable, storage, sizeof storage));
    ++controls;
}

static void refusals(void)
{
    _Alignas(16) rx_filter s;
    _Alignas(16) int8_t iq[1024] = {0};
    int16_t output[64][2], derivative[64][2];
    rx_filter_progress p = {17, 19};
    assert(!rx_filter_init(NULL, 0, 0, UINT64_C(4)<<32));
    assert(!rx_filter_init(&s, 0, 0, RX_FILTER_STEP_MIN-1));
    assert(!rx_filter_init(&s, 0, 0, RX_FILTER_STEP_MAX+1));
    assert(!rx_filter_init(&s, UINT64_MAX, 0, UINT64_C(4)<<32));
    assert(rx_filter_init(&s, 0, 0, UINT64_C(4)<<32));
    rx_filter saved = s;
    assert(!rx_filter_push(&s, iq, 513, output, NULL, 64, 0, &p));
    assert(!rx_filter_push(&s, iq, 512, output, NULL, 0, 0, &p));
    assert(!rx_filter_push(&s, iq, 512, output, NULL, 65, 0, &p));
    assert(!rx_filter_push(&s, NULL, 512, output, NULL, 64, 0, &p));
    assert(!rx_filter_push(&s, iq, 512, output, NULL, 64, 1, &p));
    assert(!rx_filter_push(&s, iq, 512, output, derivative, 1, 2, &p));
    assert(!rx_filter_push(&s, iq, 512, output, output, 64, 1, &p));
    assert(!rx_filter_push(&s, iq, 512, (int16_t (*)[2])iq, NULL, 64, 0, &p));
    assert(!rx_filter_push(&s, (int8_t *)&s, 512, output, NULL, 64, 0, &p));
    assert(!rx_filter_push(&s, iq, 512, output, NULL, 64, 0, (rx_filter_progress *)&s));
    assert(p.consumed == 17 && p.written == 19 && !memcmp(&s, &saved, sizeof s));
    s.received = UINT64_MAX; s.base = s.received;
    assert(!rx_filter_push(&s, iq, 1, output, NULL, 64, 0, &p));
    s.failed = true;
    assert(!rx_filter_push(&s, iq, 0, output, NULL, 64, 0, &p));
    assert(rx_filter_init(&s, 0, 0, UINT64_C(4)<<32));
    assert(!s.failed && rx_filter_set_step(&s, RX_FILTER_STEP_MIN));
    assert(!rx_filter_set_step(&s, RX_FILTER_STEP_MIN-1));
    /* A512-input chunk at step<4 is explicitly drained over multiple calls. */
    unsigned total = 0;
    assert(rx_filter_push(&s, iq, 512, output, NULL, 64, 0, &p));
    assert(p.consumed == 512 && p.written == 64); total += (unsigned)p.written;
    do {
        assert(rx_filter_push(&s, NULL, 0, output, NULL, 64, 0, &p));
        assert(!p.consumed); total += (unsigned)p.written;
    } while (p.written);
    assert(total == 125);
    assert(rx_filter_set_step(&s, RX_FILTER_STEP_MAX));
    controls += 20;
}

static void span_boundaries(void)
{
    rx_filter s;
    int16_t pulse[64][2], derivative[64][2];
    rx_filter_progress p;
    assert(rx_filter_init(&s, UINT64_MAX-90, UINT32_MAX, UINT64_C(4)<<32));
    s.base = UINT64_MAX-96;
    s.received = UINT64_MAX-32;
    s.count = 64;
    for (unsigned a = 0; a < 2; ++a)
        for (unsigned k = 0; k < 64; ++k) s.plane[a][k] = (int8_t)(3*k+a);
    uint64_t origin = s.origin;
    assert(rx_filter_push(&s, NULL, 0, pulse, derivative, 64, UINT64_MAX, &p));
    assert(!p.consumed && p.written == 7 && !s.failed);
    assert(s.origin == origin+4*p.written && s.fraction == UINT32_MAX);
    for (unsigned j = 0; j < p.written; ++j)
        for (unsigned a = 0; a < 2; ++a) {
            int64_t sum = 0, ds = 0;
            for (unsigned k = 0; k < 32; ++k) {
                sum += s.plane[a][7+4*j+k]*rx_filter_pulse[0][k];
                ds += s.plane[a][7+4*j+k]*rx_filter_derivative[0][k];
            }
            assert(pulse[j][a] == rounded(sum, 1) && derivative[j][a] == rounded(ds, 9));
        }
#ifdef RX_FILTER_MOCK_PIE
    _Alignas(16) int8_t input[1024];
    int16_t reference[64][2], reference_d[64][2];
    for (unsigned k = 0; k < 1024; ++k) input[k] = (int8_t)(k*37);
    assert(rx_filter_init(&s, 0, UINT32_MAX, RX_FILTER_STEP_MIN));
    assert(rx_filter_push(&s, input, 512, reference, reference_d, 64, UINT64_MAX, &p));
    for (unsigned n = 0; n < 3; ++n) {
        unsigned done = n == 0 ? 0 : n == 1 ? 7 : 63;
        assert(rx_filter_init(&s, 0, UINT32_MAX, RX_FILTER_STEP_MIN));
        memset(pulse, 0x31, sizeof pulse); memset(derivative, 0x57, sizeof derivative);
        fail_after = (int)done;
        assert(!rx_filter_push(&s, input, 512, pulse, derivative, 64, UINT64_MAX, &p));
        fail_after = -1;
        assert(s.failed && p.consumed == 512 && p.written == done);
        uint64_t expected = UINT32_MAX + done*RX_FILTER_STEP_MIN;
        assert(s.origin == expected>>32 && s.fraction == (uint32_t)expected);
        assert(!memcmp(pulse, reference, 4*done) && !memcmp(derivative, reference_d, 4*done));
        for (unsigned j = done; j < 64; ++j) {
            assert(pulse[j][0] == 0x3131 && pulse[j][1] == 0x3131);
            assert(derivative[j][0] == 0x5757 && derivative[j][1] == 0x5757);
        }
    }
#endif
}

int main(void)
{
    rx_filter_check check;
    rx_filter_preflight(&check);
    assert(check.cases == 64 && check.errors == 0);
    for (int64_t x = -100001; x < 100001; x += 7)
        for (unsigned shift = 1; shift <= 9; shift += 8)
            assert(rx_filter_round(x, shift) == rounded(x, shift));
    assert(rx_filter_round(INT64_MAX, 1) == INT16_MAX);
    assert(rx_filter_round(INT64_MIN, 1) == INT16_MIN);
    uint64_t steps[3] = {RX_FILTER_STEP_MIN, UINT64_C(4)<<32, RX_FILTER_STEP_MAX};
    for (unsigned phase = 0; phase < 65+64*3; ++phase)
        for (unsigned k = 0; k < 3; ++k)
            stream((phase+k)%32, phase, steps[k], (phase+k)%4);
    refusals();
    span_boundaries();
    printf("RX filter: %u streams/refusals, %u dot controls, state %zu B, host%s\n",
           controls, check.cases, sizeof(rx_filter), check.native ? "-mock-PIE-path" : "-scalar");
    return 0;
}
