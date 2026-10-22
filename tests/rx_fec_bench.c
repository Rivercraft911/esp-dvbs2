// Receiver FEC bench, radio off. Each PIE kernel runs against its portable C version on
// random inputs (same bytes required) and both are timed; then whole MODCOD 7 frames
// (clean, a few wrong bits, AWGN) are decoded with the PIE and the C kernels. Results are
// @JSON lines. On the board "fecbench" on the USB console reruns and ends with
// {"done":true}; in QEMU (UART console) it runs once and prints QEMU-END.
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "dvbs2.h"
#include "dvbs2rx.h"
#include "rx_fec_internal.h"

#define MODCOD 7
#define EDGES 13            /* the largest 3/4 row group: 11 first occurrences, 2 repeats */
#define RUNS 5
#define BELIEF (47 * RX_SLOT)
#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static uint32_t seed;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void fill(void *p, size_t n) { for (size_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = (uint8_t)rnd(); }
static uint32_t crc(const void *p, size_t n) { return esp_rom_crc32_le(0, p, n); }
static uint32_t now(void) { return esp_cpu_get_cycle_count(); }

static uint8_t *alloc(size_t n)
{
    uint8_t *p = heap_caps_aligned_alloc(16, n, INTERNAL);
    if (!p) printf("@{\"cmd\":\"fecbench\",\"error\":\"allocation of %u bytes failed\"}\n", (unsigned)n);
    return p;
}

/* Inputs for one kernel run, restored before each version; outputs compared after. */
static uint8_t *in, *out_c, *base;
static size_t in_bytes;

/* QEMU 9.2.2 (esp_develop_9.2.2_20260417) saturates s8 sums at -127, the TRM (1.8.76)
 * at -128: differences of only that kind are counted apart. */
static void kernel_report(const char *name, uint32_t c, uint32_t pie)
{
    unsigned diff = 0, low = 0;
    for (size_t i = 0; i < in_bytes; ++i) {
        diff += out_c[i] != base[i];
        low += out_c[i] == 0x80 && base[i] == 0x81;
    }
    printf("@{\"cmd\":\"fecbench\",\"kernel\":\"%s\",\"c_cycles\":%" PRIu32 ",\"pie_cycles\":%"
           PRIu32 ",\"same\":%s,\"diff_bytes\":%u,\"diff_minus128\":%u,\"crc32\":%" PRIu32 "}\n",
           name, c, pie, diff ? "false" : "true", diff, low, crc(base, in_bytes));
}

/* Times fn on the inputs (restored each run) as C (pie = 0) and PIE; compares the
 * whole buffer afterwards. */
typedef void (*run_fn)(int pie);
static void compare(const char *name, run_fn fn)
{
    uint32_t best[2] = {UINT32_MAX, UINT32_MAX};
    for (int pie = 0; pie < 2; ++pie) {
        for (unsigned run = 0; run < RUNS; ++run) {
            memcpy(base, in, in_bytes);
            const uint32_t t0 = now();
            fn(pie);
            const uint32_t t = now() - t0;
            best[pie] = t < best[pie] ? t : best[pie];
        }
        if (!pie) memcpy(out_c, base, in_bytes);
    }
    kernel_report(name, best[0], best[1]);
}

/* Buffer layout for the kernel tests, all inside base. */
#define B_BELIEF 0
#define B_R (B_BELIEF + BELIEF)
#define B_QTMP (B_R + 16 * (EDGES * RX_VECTORS + 1))
#define B_STATE (B_QTMP + 32 * (EDGES * RX_VECTORS + 1))
#define B_READS (B_STATE + 64 * RX_VECTORS)
#define B_WRITES (B_READS + 2 * (EDGES * RX_VECTORS + 8))
#define B_LLR (B_WRITES + 64)
#define B_HARD (B_LLR + 16224)
#define B_TOTAL (B_HARD + 2048)
#define P(type, at) ((type *)(base + (at)))
#define I(type, at) ((type *)(in + (at)))

static void run_pass1(int pie)
{
    (pie ? rx_pass1_pie : rx_pass1)(P(int8_t, B_BELIEF), P(uint16_t, B_READS), P(int8_t, B_R),
                                     P(int8_t, B_QTMP), P(int8_t, B_STATE), EDGES);
}
static void run_pass2(int pie)
{
    (pie ? rx_pass2_pie : rx_pass2)(P(int8_t, B_BELIEF), P(uint16_t, B_WRITES), P(int8_t, B_R),
                                     P(int8_t, B_QTMP), P(int8_t, B_STATE), 11 | 2 << 8);
}
static void run_tails(int pie) { (pie ? rx_tails_pie : rx_tails)(P(int8_t, B_BELIEF), P(uint16_t, B_WRITES), 11); }
static unsigned start;
static void run_repeat(int pie)
{
    (pie ? rx_repeat_pie : rx_repeat)(P(int8_t, B_BELIEF) + 3 * RX_SLOT, P(int8_t, B_BELIEF) + 45 * RX_SLOT, start);
}
static unsigned gain;
static void run_scale(int pie) { (pie ? rx_scale_pie : rx_scale)(P(int8_t, B_BELIEF), RX_GROUPS, gain); }
static void run_moments(int pie)
{
    (pie ? rx_moments_pie : rx_moments)(P(int8_t, B_LLR + 3), RX_STAT_VECTORS, P(uint32_t, B_HARD));
}
static void run_parity(int pie)
{
    *P(int, B_HARD) = (pie ? rx_parity_pie : rx_parity)(P(int8_t, B_BELIEF), P(uint16_t, B_WRITES), EDGES) != 0;
}
static void run_xor(int pie)
{
    (pie ? rx_xor_pie : rx_xor)(P(uint8_t, B_BELIEF), P(uint8_t, B_QTMP), 91);
}
static void run_hard(int pie)
{
    (pie ? rx_hard_pie : rx_hard)(P(int8_t, B_BELIEF), P(uint16_t, B_READS), 33, P(uint8_t, B_HARD));
}

static void kernels(void)
{
    /* pass 1: random beliefs (full int8 range), messages and window offsets. */
    seed = 1;
    in_bytes = B_TOTAL;
    fill(in, B_TOTAL);
    for (unsigned i = 0; i < EDGES * RX_VECTORS + 8; ++i) I(uint16_t, B_READS)[i] = rnd() % (45 * RX_SLOT);
    compare("pass1", run_pass1);
    /* pass 2: consistent |q| and q, min1 <= min2, 11 slots and 2 scratch slots. */
    seed = 2;
    fill(in, B_TOTAL);
    int8_t *q = I(int8_t, B_QTMP), *state = I(int8_t, B_STATE);
    for (unsigned i = 0; i < 16 * (EDGES * RX_VECTORS + 1); ++i) {
        const int x = q[32 * (i / 16) + 16 + i % 16];
        q[32 * (i / 16) + i % 16] = (int8_t)(x < 0 ? (x == -128 ? 127 : -x) : x);
    }
    for (unsigned i = 0; i < 16 * RX_VECTORS; ++i) {
        int8_t *s = state + 64 * (i / 16) + i % 16;
        const unsigned a = rnd() % 128, b = rnd() % 128;
        s[0] = (int8_t)(a < b ? a : b);
        s[16] = (int8_t)(a < b ? b : a);
    }
    for (unsigned j = 0; j < EDGES; ++j) I(uint16_t, B_WRITES)[j] = (uint16_t)(RX_SLOT * (3 * j + 1));
    compare("pass2", run_pass2);
    compare("tails", run_tails);
    for (unsigned k = 0; k < 6; ++k) {
        start = k < 4 ? (unsigned[]){0, 7, 8, 359}[k] : rnd() % 360;
        char name[24];
        snprintf(name, sizeof name, "repeat_%u", start);
        compare(name, run_repeat);
    }
    for (unsigned k = 0; k < 4; ++k) {
        gain = (unsigned[]){1, 9, 16, 64}[k];
        char name[16];
        snprintf(name, sizeof name, "scale_%u", gain);
        compare(name, run_scale);
    }
    compare("moments", run_moments);
    /* parity: random signs (odd somewhere), then all positive with one negative byte. */
    for (unsigned j = 0; j < EDGES; ++j) {
        I(uint16_t, B_WRITES)[2 * j] = (uint16_t)(RX_SLOT * (3 * j + 1));
        I(uint16_t, B_WRITES)[2 * j + 1] = (uint16_t)(rnd() % 360);
    }
    compare("parity_random", run_parity);
    for (unsigned i = 0; i < BELIEF; ++i) in[B_BELIEF + i] &= 0x7f;
    compare("parity_even", run_parity);
    in[B_BELIEF + RX_SLOT + 100] = 0x80;
    compare("parity_one", run_parity);
    for (unsigned g = 0; g < 33; ++g) I(uint16_t, B_READS)[g] = (uint16_t)(rnd() % 360);
    fill(in + B_BELIEF, BELIEF);
    compare("hard", run_hard);
    compare("xor", run_xor);
}

/* ---------------------------------------------------------------- frames */

static dvbs2_workspace encoder_ws;

/* One MODCOD 7 codeword from a seeded payload; returns its bits (one per byte). */
static void codeword(uint8_t *bits, uint8_t *payload)
{
    const dvbs2_mode *mode = dvbs2_find_mode_short(MODCOD);
    seed = 7;
    fill(payload, (mode->kbch - 80) / 8);
    dvbs2_bb_gcs(&encoder_ws, mode, payload, mode->kbch - 80);
    dvbs2_bb_scramble(&encoder_ws, mode);
    dvbs2_bch(&encoder_ws, mode);
    dvbs2_ldpc(&encoder_ws, mode);
    for (unsigned i = 0; i < RX_SHORT_BITS; ++i) bits[i] = (encoder_ws.bits[i / 8] >> (7 - i % 8)) & 1;
}

static float gaussian(void)
{
    const float u = ((rnd() >> 8) + 1) * (1.0f / 16777217.0f), v = (rnd() >> 8) * (1.0f / 16777216.0f);
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

/* Sweeping 128 KB of PSRAM evicts the 32 KB data cache: flash constants and PSRAM
 * work areas are then cold. Without PSRAM (QEMU) nothing is evicted. */
static volatile uint8_t *evictor;
#define EVICT_BYTES (128 * 1024)
static void evict(void)
{
    uint32_t sum = 0;
    for (unsigned i = 0; evictor && i < EVICT_BYTES; i += 16) sum += evictor[i];
    __asm__ volatile("" :: "r"(sum));
}

typedef struct {
    int8_t *llr;
    uint8_t *hard, *bits, *payload;
    uint8_t hard_in[RX_HARD_BYTES] __attribute__((aligned(16)));
    uint32_t want;
    unsigned bytes;
} frame_set_t;

static uint32_t decode(rx_fec_t *f, frame_set_t *x, int *length, uint32_t *sum, rx_fec_stats_t *st)
{
    memcpy(x->hard, x->hard_in, RX_HARD_BYTES);
    rx_frame_t frame = {.llr = x->llr, .hard = x->hard, .modcod = MODCOD, .type = 3};
    const uint8_t *p = NULL;
    const uint32_t t0 = now();
    *length = rx_fec_frame(f, &frame, &p, st);
    const uint32_t t = now() - t0;
    *sum = *length >= 0 ? crc(p, *length) : 0;
    return t;
}

/* Frame c: 0 clean, 1/2/3 one, 8 or 64 weak wrong bits, 4/5/6 AWGN at 6.0/4.5/4.0 dB,
 * 7 a 600-bit erasure. LLRs at 24 per unit amplitude, hard bits their signs. */
static const char *const names[] = {"clean", "wrong_1", "wrong_8", "wrong_64", "awgn_6.0dB",
                                    "awgn_4.5dB", "awgn_4.0dB", "erased"};
static void make_frame(frame_set_t *x, unsigned c)
{
    seed = 100 + c;
    const float esn0 = c == 4 ? 6.0f : c == 5 ? 4.5f : 4.0f, sigma = sqrtf(0.5f / powf(10, esn0 / 10));
    for (unsigned i = 0; i < RX_SHORT_BITS; ++i) {
        float y = x->bits[i] ? -1.0f : 1.0f;
        if (c >= 4 && c <= 6) y += 1.41421356f * sigma * gaussian();
        const float v = roundf(24 * y);
        x->llr[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
    const unsigned wrong = c == 1 ? 1 : c == 2 ? 8 : c == 3 ? 64 : 0;
    for (unsigned k = 0; k < wrong; ++k) {
        const unsigned i = rnd() % RX_SHORT_BITS;
        x->llr[i] = (int8_t)(x->bits[i] ? 4 : -4);
    }
    if (c == 7) memset(x->llr + 3000, 0, 600);
    memset(x->hard_in, 0, RX_HARD_BYTES);
    for (unsigned i = 0; i < RX_SHORT_BITS; ++i) x->hard_in[i / 8] |= (x->llr[i] < 0) << (7 - i % 8);
}

/* The clean path: first call after init, warm (back to back) and cold (data cache
 * evicted before each call), and the encoder stages alone. */
static void clean(rx_fec_t *f, frame_set_t *x)
{
    make_frame(x, 0);
    int length;
    uint32_t sum, first, warm = UINT32_MAX, cold = UINT32_MAX, cold_max = 0;
    rx_fec_stats_t st;
    first = decode(f, x, &length, &sum, &st);
    for (unsigned run = 0; run < 2 * RUNS; ++run) {
        const uint32_t t = decode(f, x, &length, &sum, &st);
        warm = t < warm ? t : warm;
    }
    for (unsigned run = 0; run < 2 * RUNS; ++run) {
        evict();
        const uint32_t t = decode(f, x, &length, &sum, &st);
        cold = t < cold ? t : cold;
        cold_max = t > cold_max ? t : cold_max;
    }
    uint32_t ldpc = UINT32_MAX, bch = UINT32_MAX, copy = UINT32_MAX;
    for (unsigned run = 0; run < RUNS; ++run) {
        const uint32_t t0 = now();
        memcpy(f->bits->bits, x->hard_in, f->nbch / 8);
        const uint32_t t1 = now();
        dvbs2_fast_ldpc(f->encoder, f->bits);
        const uint32_t t2 = now();
        dvbs2_fast_bch(f->encoder, f->bits);
        const uint32_t t3 = now();
        copy = t1 - t0 < copy ? t1 - t0 : copy;
        ldpc = t2 - t1 < ldpc ? t2 - t1 : ldpc;
        bch = t3 - t2 < bch ? t3 - t2 : bch;
    }
    printf("@{\"cmd\":\"fecbench\",\"frame\":\"clean\",\"first\":%" PRIu32 ",\"warm\":%" PRIu32
           ",\"cold\":%" PRIu32 ",\"cold_max\":%" PRIu32 ",\"copy\":%" PRIu32 ",\"ldpc\":%" PRIu32
           ",\"bch\":%" PRIu32 ",\"length\":%d,\"payload_ok\":%s}\n", first, warm, cold, cold_max,
           copy, ldpc, bch, length, sum == x->want ? "true" : "false");
    /* W1's paired BCH table: 4 KB more internal RAM. */
    uint8_t *low = alloc(DVBS2_BCH_PAIR_BYTES);
    if (low && !dvbs2_fast_cache_bch_pair(f->encoder, low)) {
        bch = warm = UINT32_MAX;
        for (unsigned run = 0; run < 2 * RUNS; ++run) {
            uint32_t t = decode(f, x, &length, &sum, &st);
            warm = t < warm ? t : warm;
            memcpy(f->bits->bits, x->hard_in, f->kbch / 8);
            t = now();
            dvbs2_fast_bch(f->encoder, f->bits);
            t = now() - t;
            bch = t < bch ? t : bch;
        }
        printf("@{\"cmd\":\"fecbench\",\"frame\":\"clean_bch_pair\",\"warm\":%" PRIu32 ",\"bch\":%"
               PRIu32 ",\"payload_ok\":%s}\n", warm, bch, sum == x->want ? "true" : "false");
    }
    heap_caps_free(low);
    rx_fec_init(f, MODCOD, f->belief);   /* back to the plain resident table */
}

/* Damaged frames with the work area at 'placement', cold cache each time; C too if asked. */
static void frames(rx_fec_t *f, frame_set_t *x, const char *placement, bool with_c)
{
    for (unsigned c = 1; c < sizeof names / sizeof *names; ++c) {
        make_frame(x, c);
        uint32_t cycles[2] = {0}, sums[2] = {0};
        int length[2] = {0};
        rx_fec_stats_t st[2] = {0};
        for (int pie = 1; pie >= !with_c; --pie) {
            f->pie = (unsigned)pie;
            evict();
            cycles[pie] = decode(f, x, &length[pie], &sums[pie], &st[pie]);
        }
        f->pie = 1;
        printf("@{\"cmd\":\"fecbench\",\"frame\":\"%s\",\"work\":\"%s\",\"pie_cycles\":%" PRIu32,
               names[c], placement, cycles[1]);
        if (with_c) {
            printf(",\"c_cycles\":%" PRIu32 ",\"same\":%s", cycles[0],
                   length[0] == length[1] && sums[0] == sums[1] && st[0].iterations == st[1].iterations
                       ? "true" : "false");
        }
        printf(",\"length\":%d,\"payload_ok\":%s,\"iterations\":%u,\"failed_checks\":%u}\n",
               length[1], sums[1] == x->want ? "true" : "false", st[1].iterations, st[1].failed_checks);
    }
    /* One iteration: a hopeless frame decoded with 1 and 2 iterations allowed. */
    seed = 99;
    memset(x->llr, 0, RX_SHORT_BITS);
    for (unsigned i = 0; i < RX_SHORT_BITS; i += 3) x->llr[i] = (int8_t)(rnd() % 48) - 24;
    memset(x->hard_in, 0, RX_HARD_BYTES);
    for (unsigned i = 0; i < RX_SHORT_BITS; ++i) x->hard_in[i / 8] |= (x->llr[i] < 0) << (7 - i % 8);
    uint32_t t[2];
    for (unsigned n = 1; n <= 2; ++n) {
        f->max_iterations = n;
        uint32_t best = UINT32_MAX, sum;
        int length;
        rx_fec_stats_t st;
        for (unsigned run = 0; run < RUNS; ++run) {
            const uint32_t dt = decode(f, x, &length, &sum, &st);
            best = dt < best ? dt : best;
        }
        t[n - 1] = best;
    }
    f->max_iterations = 30;
    printf("@{\"cmd\":\"fecbench\",\"work\":\"%s\",\"iteration_cycles\":%" PRIu32
           ",\"setup_cycles\":%" PRIu32 "}\n", placement, t[1] - t[0], 2 * t[0] - t[1]);
}

/* PIE pipeline: cycles per repetition of a short sequence, 64 repetitions in a zero-overhead
 * loop, minus nothing (the loop itself is free). */
#define MICRO(name, setup, body) do {                                              \
        static uint8_t buf[256] __attribute__((aligned(16)));                       \
        uint32_t t0, t1;                                                            \
        __asm__ volatile(setup "\n rsr.ccount %0\n movi a8, 64\n loopnez a8, 1f\n"  \
                         body "\n1:\n rsr.ccount %1"                               \
                         : "=&r"(t0), "=&r"(t1) : "r"(buf) : "a8", "a9", "a10", "memory"); \
        printf("%s\"%s\":%.2f", first ? "" : ",", name, (t1 - t0) / 64.0f);       \
        first = false;                                                              \
    } while (0)

static void micro(void)
{
    bool first = true;
    printf("@{\"cmd\":\"fecbench\",\"cycles_per_repetition\":{");
    MICRO("vadds_chain", "", "ee.vadds.s8 q0, q0, q1");
    MICRO("vadds_independent", "", "ee.vadds.s8 q0, q2, q1\n ee.vadds.s8 q3, q2, q1");
    MICRO("xorq_chain", "", "ee.xorq q0, q0, q1");
    MICRO("vcmp_and_chain", "", "ee.vcmp.lt.s8 q0, q1, q2\n ee.andq q0, q0, q3");
    MICRO("vld_use", "mov a9, %2", "ee.vld.128.ip q0, a9, 0\n ee.vadds.s8 q1, q0, q2");
    MICRO("vld_gap_use", "mov a9, %2", "ee.vld.128.ip q0, a9, 0\n ee.vadds.s8 q3, q4, q2\n ee.vadds.s8 q1, q0, q2");
    MICRO("usar_vld_src", "mov a9, %2", "ee.ld.128.usar.ip q0, a9, 0\n ee.vld.128.ip q1, a9, 0\n ee.src.q q2, q0, q1");
    MICRO("usar_vld_src_use", "mov a9, %2", "ee.ld.128.usar.ip q0, a9, 0\n ee.vld.128.ip q1, a9, 0\n ee.src.q q2, q0, q1\n ee.vadds.s8 q3, q2, q2");
    MICRO("l16ui_add_usar", "mov a9, %2", "l16ui a10, a9, 0\n add a10, a10, a9\n ee.ld.128.usar.ip q0, a10, 0");
    MICRO("src_ld", "mov a9, %2", "ee.src.q.ld.ip q6, a9, 0, q5, q4");
    MICRO("vsubs_ld_incp", "mov a9, %2", "ee.vsubs.s8.ld.incp q5, a9, q6, q6, q7\n addi a9, a9, -16");
    MICRO("vmax_st_incp", "mov a9, %2", "ee.vmax.s8.st.incp q7, a9, q4, q0, q7\n addi a9, a9, -16");
    MICRO("vst", "mov a9, %2", "ee.vst.128.ip q0, a9, 0");
    MICRO("vmul_s8", "", "ee.vmul.s8 q0, q1, q2");
    MICRO("pass1_body", "mov a9, %2\n mov a10, %2",
          "ee.src.q.ld.ip q6, a10, 0, q5, q4\n l16ui a9, a10, 0\n add a9, a9, a10\n"
          " ee.vsubs.s8 q7, q6, q5\n ee.vsubs.s8 q6, q5, q6\n ee.xorq q3, q3, q5\n"
          " ee.ld.128.usar.ip q5, a9, 0\n ee.vmax.s8 q7, q6, q7\n ee.xorq q2, q2, q6\n"
          " ee.vmax.s8.st.incp q7, a10, q4, q0, q7\n ee.vmin.s8.st.incp q6, a10, q1, q1, q4\n"
          " ee.vmin.s8.ld.incp q4, a9, q0, q0, q7\n addi a10, a10, -32");
    printf("}}\n");
}

/* EE.VADDS.S8 of -100 and -100 in every lane: -128 per the TRM. */
static void probe(void)
{
    static int8_t x[16] __attribute__((aligned(16)));
    memset(x, -100, sizeof x);
    __asm__ volatile("ee.vld.128.ip q0, %0, 0\n ee.vadds.s8 q1, q0, q0\n ee.vst.128.ip q1, %0, 0"
                     :: "r"(x) : "memory");
    printf("@{\"cmd\":\"fecbench\",\"vadds_s8_minus200\":%d}\n", x[0]);
}

static void bench(void)
{
    const size_t size = rx_fec_size(MODCOD), work_size = rx_fec_work_size(MODCOD);
    printf("@{\"cmd\":\"fecbench\",\"modcod\":%u,\"fec_bytes\":%u,\"work_bytes\":%u,\"free_internal\":%u,"
           "\"largest_internal\":%u}\n", MODCOD, (unsigned)size, (unsigned)work_size,
           (unsigned)heap_caps_get_free_size(INTERNAL), (unsigned)heap_caps_get_largest_free_block(INTERNAL));
    in = alloc(B_TOTAL);
    out_c = alloc(B_TOTAL);
    base = alloc(B_TOTAL);
    rx_fec_t *f = (rx_fec_t *)alloc(size);
    void *work = alloc(work_size);
    void *external = heap_caps_aligned_alloc(16, work_size, MALLOC_CAP_SPIRAM);
    evictor = heap_caps_malloc(EVICT_BYTES, MALLOC_CAP_SPIRAM);
    frame_set_t *x = (frame_set_t *)alloc(sizeof *x);
    if (x) {
        x->hard = alloc(RX_HARD_BYTES);
        x->bits = alloc(RX_SHORT_BITS);
        x->payload = alloc(2048);
        x->llr = (int8_t *)alloc(RX_SHORT_BITS);
    }
    probe();
    micro();
    if (in && out_c && base && f && work && x && x->hard && x->bits && x->payload && x->llr) {
        kernels();
        codeword(x->bits, x->payload);
        x->bytes = (dvbs2_find_mode_short(MODCOD)->kbch - 80) / 8;
        x->want = crc(x->payload, x->bytes);
        if (rx_fec_init(f, MODCOD, work)) {
            printf("@{\"cmd\":\"fecbench\",\"error\":\"init\"}\n");
        } else {
            clean(f, x);
            frames(f, x, "internal", true);
            if (external && !rx_fec_init(f, MODCOD, external)) {
                frames(f, x, "psram", false);
                /* Parts of the PSRAM work area moved to internal RAM (the internal work
                 * area is free now): beliefs; beliefs, q and state; messages. */
                int8_t *b = f->belief, *q = f->qtmp, *st = f->state, *r = f->r;
                const size_t beliefs = (size_t)(r - b), qtmp = 32 * (f->most * RX_VECTORS + 1);
                memcpy(work, b, beliefs);
                f->belief = (int8_t *)work;
                frames(f, x, "psram_beliefs_internal", false);
                f->qtmp = (int8_t *)work + beliefs;
                f->state = (int8_t *)work + beliefs + qtmp;
                frames(f, x, "psram_beliefs_q_state_internal", false);
                f->belief = b, f->qtmp = q, f->state = st;
                f->r = (int8_t *)work;
                frames(f, x, "psram_messages_internal", false);
                f->r = r;
            }
        }
    }
    if (x) {
        heap_caps_free(x->hard); heap_caps_free(x->bits); heap_caps_free(x->payload);
        heap_caps_free(x->llr);
    }
    heap_caps_free(x); heap_caps_free(in); heap_caps_free(out_c); heap_caps_free(base);
    heap_caps_free(f); heap_caps_free(work); heap_caps_free(external);
    heap_caps_free((void *)evictor);
    printf("@{\"cmd\":\"fecbench\",\"done\":true}\n");
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static int console_put(char c, FILE *stream)
{
    (void)stream;
    return usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY) == 1 ? 0 : EOF;
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    static FILE output = FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);
    stdout = &output;
    setvbuf(stdin, NULL, _IONBF, 0);
    char line[32];
    while (true) {
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(1); continue; }
        if (!strncmp(line, "fecbench", 8)) bench();
        else printf("@{\"error\":\"commands: fecbench\",\"done\":true}\n");
    }
}
#else
void app_main(void) { bench(); puts("QEMU-END"); }
#endif
