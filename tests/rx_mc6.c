/* Host controls: natural-order scalar graph oracle, independent edge messages,
 * and exclude-one-edge leaf arithmetic. No expected payload enters decoding. */
#include "rx_mc6.h"
#include "mc6_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void read_exact(void *p, size_t size)
{
    assert(fread(p, 1, size, stdin) == size);
}

static int clamp(int x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

static unsigned magnitude(int x)
{
    return x == -32768 ? 32767U : (unsigned)(x < 0 ? -x : x);
}

static unsigned normalized(unsigned x)
{
    x = x * 3U / 4U;
    return x < 127 ? x : 127;
}

static uint32_t random32(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void leaf_controls(void)
{
    uint32_t seed = 0x3f91203U;
    for (unsigned trial = 0; trial < 512; ++trial) {
        _Alignas(16) rx_mc6_tile w;
        int16_t original[10][8];
        for (unsigned j = 0; j < 4; ++j) w.before[j] = w.after[j] = 0x6d6d6d6dU;
        for (unsigned e = 0; e < 10; ++e)
            for (unsigned lane = 0; lane < 8; ++lane) {
                int value = (int)(random32(&seed) & 65535U) - 32768;
                if (trial < 8) value = (int[]){0, 1, -1, 127, -127, 32767, -32768, 32766}[trial];
                if (trial == 8) value = e & 1 ? -32768 : 32767;
                if (trial == 9) value = e < 2 ? (lane & 1 ? -8 : 8) : 2000;
                original[e][lane] = w.q[e][lane] = (int16_t)value;
            }
        rx_mc6_leaf(&w);
        for (unsigned lane = 0; lane < 8; ++lane) {
            unsigned first = 32767, second = 32767, index = 0, signs = 0;
            for (unsigned e = 0; e < 10; ++e) {
                unsigned a = magnitude(original[e][lane]);
                if (a <= first) { second = first; first = a; index = e; }
                else if (a < second) second = a;
                unsigned m = 32767, negative = 0;
                for (unsigned other = 0; other < 10; ++other) {
                    if (other == e) continue;
                    unsigned b = magnitude(original[other][lane]);
                    if (b < m) m = b;
                    negative ^= original[other][lane] < 0;
                }
                int r = (int)normalized(m);
                if (negative) r = -r;
                assert(w.q[e][lane] == clamp(original[e][lane] + r));
                signs |= negative << e;
            }
            assert(w.magnitudes[lane] == (normalized(first) | normalized(second) << 8));
            assert(w.metadata[lane] == (signs | index << 10));
        }
        for (unsigned j = 0; j < 4; ++j)
            assert(w.before[j] == 0x6d6d6d6dU && w.after[j] == 0x6d6d6d6dU);
    }
}

typedef struct {
    uint16_t variable[RX_MC6_CHECKS][10];
    uint8_t degree[RX_MC6_CHECKS], visited[RX_MC6_CHECKS];
    int16_t belief[RX_MC6_N];
    int8_t message[RX_MC6_CHECKS][10];
    rx_mc6_check state[RX_MC6_CHECKS];
} reference;

/* Raw Annex B.6 variable-to-check bases, not the production check-row tokens. */
static void reference_graph(reference *r)
{
    uint8_t degrees[120];
    read_exact(degrees, sizeof degrees);
    for (unsigned group = 0; group < 120; ++group)
        for (unsigned e = 0; e < degrees[group]; ++e) {
            uint8_t bytes[2];
            read_exact(bytes, 2);
            unsigned base = bytes[0] | (unsigned)bytes[1] << 8;
            assert(base < RX_MC6_CHECKS);
            for (unsigned j = 0; j < 360; ++j) {
                unsigned check = (base + 60U * j) % RX_MC6_CHECKS;
                unsigned n = r->degree[check]++;
                assert(n < 8);
                r->variable[check][n] = (uint16_t)(360U * group + j);
            }
        }
    for (unsigned k = 0; k < RX_MC6_CHECKS; ++k) {
        assert(r->degree[k] == 8);
        r->variable[k][r->degree[k]++] = (uint16_t)(RX_MC6_K + k);
        if (k) r->variable[k][r->degree[k]++] = (uint16_t)(RX_MC6_K + k - 1);
    }
}

static void reference_tile(reference *r, unsigned layer, unsigned column)
{
    unsigned lanes = layer ? 8U : 4U;
    uint16_t touched[80];
    unsigned count = 0;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        unsigned k = layer + 60U * (column + lane), degree = r->degree[k];
        int q[10];
        unsigned first = 32767, second = 32767, index = 0, parity = 0;
        for (unsigned e = 0; e < degree; ++e) {
            unsigned v = r->variable[k][e];
            for (unsigned j = 0; j < count; ++j) assert(touched[j] != v);
            touched[count++] = (uint16_t)v;
            q[e] = clamp(r->belief[v] - r->message[k][e]);
            unsigned a = magnitude(q[e]);
            if (a <= first) { second = first; first = a; index = e; }
            else if (a < second) second = a;
            parity ^= q[e] < 0;
        }
        unsigned signs = 0, m1 = normalized(first), m2 = normalized(second);
        for (unsigned e = 0; e < degree; ++e) {
            unsigned negative = parity ^ (q[e] < 0);
            int next = (int)(e == index ? m2 : m1);
            if (negative) next = -next;
            r->message[k][e] = (int8_t)next;
            r->belief[r->variable[k][e]] = (int16_t)clamp(q[e] + next);
            signs |= negative << e;
        }
        r->state[k] = (rx_mc6_check){(uint16_t)(m1 | m2 << 8), (uint16_t)(signs | index << 10)};
        r->visited[k] = 1;
    }
}

static void compare(const reference *r, const rx_mc6 *d, unsigned last_layer, unsigned last_column)
{
    assert(!memcmp(r->belief, d->belief, RX_MC6_K * sizeof(int16_t)));
    for (unsigned layer = 0; layer < 60; ++layer)
        for (unsigned column = 0; column < 360; ++column) {
            unsigned k = 60U * column + layer, at = 360U * layer + column;
            assert(r->belief[RX_MC6_K + k] == d->belief[RX_MC6_K + at]);
            if (!r->visited[k]) continue;
            assert(r->state[k].magnitudes == d->checks[at].magnitudes);
            assert(r->state[k].metadata == d->checks[at].metadata);
            if (layer != last_layer || column < last_column ||
                column >= last_column + (layer ? 8U : 4U)) continue;
            for (unsigned e = 0; e < r->degree[k]; ++e) {
                unsigned meta = d->checks[at].metadata, mags = d->checks[at].magnitudes;
                int old = (int)((meta >> 10) == e ? mags >> 8 : mags & 255U);
                if (meta & (1U << e)) old = -old;
                assert(old == r->message[k][e]);
            }
        }
}

static void state_controls(unsigned iterations)
{
    reference *r = calloc(1, sizeof *r);
    int8_t *llr = malloc(RX_MC6_N);
    int16_t *belief = malloc(2U * RX_MC6_N), *other = malloc(2U * RX_MC6_N);
    rx_mc6_check *checks = malloc(4U * RX_MC6_CHECKS), *alternate = malloc(4U * RX_MC6_CHECKS);
    assert(r && llr && belief && other && checks && alternate);
    reference_graph(r);
    read_exact(llr, RX_MC6_N);
    assert(getchar() == EOF);
    for (unsigned n = 0; n < RX_MC6_N; ++n) r->belief[n] = llr[n];
    memset(checks, 0xa5, 4U * RX_MC6_CHECKS);
    memset(alternate, 0x5a, 4U * RX_MC6_CHECKS);
    rx_mc6 d, resumed;
    assert(rx_mc6_init(&d, belief, checks, llr));
    assert(rx_mc6_init(&resumed, other, alternate, llr));
    assert(checks[0].magnitudes == 0xa5a5U && alternate[0].metadata == 0x5a5aU);
    for (unsigned tile = 0; tile < iterations * RX_MC6_TILES; ++tile) {
        unsigned last_layer = d.layer, last_column = d.column;
        reference_tile(r, d.layer, d.column);
        assert(rx_mc6_step(&d, 1) == 1 && !d.failed);
        compare(r, &d, last_layer, last_column);
    }
    const unsigned budgets[] = {0, 1, 89, 134, 2744, 7, 90, 2745};
    unsigned left = iterations * RX_MC6_TILES;
    for (unsigned n = 0; left; ++n) {
        unsigned take = budgets[n % (sizeof budgets / sizeof budgets[0])];
        if (take > left) take = left;
        rx_mc6 saved = resumed;
        assert(rx_mc6_step(&resumed, take) == take);
        if (!take) assert(!memcmp(&saved, &resumed, sizeof saved));
        left -= take;
    }
    assert(d.iteration == iterations && !d.layer && !d.column && !d.failed);
    assert(resumed.iteration == iterations && !resumed.layer && !resumed.column && !resumed.failed);
    assert(!memcmp(belief, other, 2U * RX_MC6_N));
    assert(!memcmp(checks, alternate, 4U * RX_MC6_CHECKS));
    uint8_t bits[RX_MC6_WORD_BYTES];
    assert(rx_mc6_pack(&d, bits));
    assert(fwrite(bits, 1, sizeof bits, stdout) == sizeof bits);
    fprintf(stderr, "{\"tiles\":%u,\"iterations\":%u,\"all_beliefs_messages_per_tile\":true}\n",
            iterations * RX_MC6_TILES, iterations);
    free(r); free(llr); free(belief); free(other); free(checks); free(alternate);
}

static void ownership_controls(void)
{
    int16_t *belief = calloc(RX_MC6_N, 2);
    rx_mc6_check *checks = malloc(4U * RX_MC6_CHECKS + 4U);
    int8_t *llr = calloc(RX_MC6_N, 1);
    rx_mc6 d;
    assert(belief && checks && llr);
    memset(checks, 0xa5, 4U * RX_MC6_CHECKS);
    assert(!rx_mc6_init(&d, belief, checks, (const int8_t *)belief));
    assert(checks[0].metadata == 0xa5a5 && belief[0] == 0);
    assert(!rx_mc6_init((rx_mc6 *)belief, belief, checks, llr));
    assert(!rx_mc6_init(&d, belief, (rx_mc6_check *)((uint8_t *)checks + 2), llr));
    assert(rx_mc6_init(&d, belief, checks, llr));
    assert(!rx_mc6_pack(&d, (uint8_t *)belief));
    assert(rx_mc6_step(&d, 1) == 1);
    d.iteration = 1;
    checks[d.column].metadata = 0xc000;
    int16_t snapshot[16];
    memcpy(snapshot, belief, sizeof snapshot);
    assert(rx_mc6_step(&d, 1) == 0 && d.failed);
    assert(!memcmp(snapshot, belief, sizeof snapshot));
    /* Defensive full-S16 check0: dummy edge must never become real argmin9. */
    assert(rx_mc6_init(&d, belief, checks, llr));
    for (unsigned n = 0; n < RX_MC6_N; ++n) belief[n] = 32767;
    assert(rx_mc6_step(&d, 1) == 1 && !d.failed);
    assert(checks[0].magnitudes == 0x7f7fU && checks[0].metadata == (8U << 10));
    assert(rx_mc6_step(&d, RX_MC6_TILES - 1) == RX_MC6_TILES - 1 && !d.failed);
    assert(d.iteration == 1 && rx_mc6_step(&d, 1) == 1 && !d.failed);
    assert((checks[0].metadata >> 10) < 9 && !(checks[0].metadata & 512U));
    uint8_t bits[RX_MC6_WORD_BYTES] = {0}, payload[32];
    memset(payload, 0x6d, sizeof payload);
    size_t bytes = 123;
    assert(!rx_mc6_extract(bits, payload, sizeof payload, &bytes));
    assert(bytes == 123 && payload[0] == 0x6d);
    assert(!rx_mc6_check_word(bits, (rx_mc6_result *)bits));
    free(belief); free(checks); free(llr);
}

static void decode(unsigned iterations)
{
    int8_t *llr = malloc(RX_MC6_N);
    int16_t *belief = malloc(2U * RX_MC6_N);
    rx_mc6_check *checks = malloc(4U * RX_MC6_CHECKS);
    uint8_t bits[RX_MC6_WORD_BYTES], payload[RX_MC6_PAYLOAD_BYTES];
    assert(llr && belief && checks);
    read_exact(llr, RX_MC6_N);
    assert(getchar() == EOF);
    rx_mc6 d;
    assert(rx_mc6_init(&d, belief, checks, llr));
    assert(rx_mc6_pack(&d, bits));
    rx_mc6_result result;
    bool valid = rx_mc6_check_word(bits, &result);
    for (unsigned i = 0; !valid && i < iterations; ++i) {
        assert(rx_mc6_step(&d, RX_MC6_TILES) == RX_MC6_TILES && !d.failed);
        assert(rx_mc6_pack(&d, bits));
        valid = rx_mc6_check_word(bits, &result);
    }
    size_t count = 0;
    memset(payload, 0x6d, sizeof payload);
    size_t refused_count = 123;
    assert(!rx_mc6_extract(bits, payload, valid ? result.payload_bytes - 1U : sizeof payload,
                           &refused_count));
    assert(refused_count == 123);
    for (unsigned n = 0; n < sizeof payload; ++n) assert(payload[n] == 0x6d);
    if (valid) assert(rx_mc6_extract(bits, payload, sizeof payload, &count));
    assert(fwrite(bits, 1, sizeof bits, stdout) == sizeof bits);
    assert(fwrite(payload, 1, count, stdout) == count);
    fprintf(stderr, "{\"valid\":%s,\"iterations\":%u,\"H\":%u,\"bch\":%s,\"header\":%s,\"bytes\":%zu}\n",
            valid ? "true" : "false", d.iteration, result.unsatisfied,
            result.bch_ok ? "true" : "false", result.header_ok ? "true" : "false", count);
    free(llr); free(belief); free(checks);
}

static void check_packed(void)
{
    uint8_t bits[RX_MC6_WORD_BYTES];
    read_exact(bits, sizeof bits);
    assert(getchar() == EOF);
    rx_mc6_result result;
    bool valid = rx_mc6_check_word(bits, &result);
    printf("{\"valid\":%s,\"H\":%u,\"bch\":%s,\"header\":%s}\n",
           valid ? "true" : "false", result.unsatisfied,
           result.bch_ok ? "true" : "false", result.header_ok ? "true" : "false");
}

static void basis_checks(unsigned count)
{
    assert(count <= RX_MC6_N);
    uint8_t bits[RX_MC6_WORD_BYTES] = {0};
    for (unsigned n = 0; n < count; ++n) {
        bits[n / 8] = (uint8_t)(128U >> (n % 8U));
        rx_mc6_result result;
        assert(!rx_mc6_check_word(bits, &result));
        assert(result.bch_ok == (n >= RX_MC6_K));
        assert(result.unsatisfied <= 13);
        uint8_t value[2] = {(uint8_t)result.unsatisfied, (uint8_t)(result.unsatisfied >> 8)};
        assert(fwrite(value, 1, sizeof value, stdout) == sizeof value);
        bits[n / 8] = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        leaf_controls();
        ownership_controls();
        puts("MC6: 512 independent full-range leaf vectors; ownership/refusal controls PASS");
    } else if (argc == 2 && !strcmp(argv[1], "check")) check_packed();
    else if (argc == 3 && !strcmp(argv[1], "basis")) basis_checks((unsigned)strtoul(argv[2], NULL, 10));
    else if (argc == 3 && !strcmp(argv[1], "state")) state_controls((unsigned)strtoul(argv[2], NULL, 10));
    else if (argc == 3 && !strcmp(argv[1], "decode")) decode((unsigned)strtoul(argv[2], NULL, 10));
    else return 2;
    return 0;
}
