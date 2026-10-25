// Receiver FEC bench, radio off. Each PIE kernel runs against its portable C version on
// random inputs (same bytes required) and both are timed; then whole MODCOD 7 frames
// (clean, a few wrong bits, AWGN) are decoded with the PIE and the C kernels. Results are
// @JSON lines. On the board "fecbench" on the USB console reruns and ends with
// {"done":true}; in QEMU (UART console) it runs once and prints QEMU-END.
// "curve MODCOD ESN0_CDB FRAMES [LEVELS]" decodes noisy frames of any short MODCOD the way the live
// receiver does (see curve()).
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp32s3/rom/cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "demod_pie.h"
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
#define B_SAVED (B_R + 16 * (EDGES * RX_VECTORS + 1))
#define B_STATE (B_SAVED + 16 * 2 * RX_VECTORS)
#define B_PACKED (B_STATE + 64 * RX_VECTORS + 16)
#define B_READS (B_PACKED + 16 * RX_VECTORS * 5)
#define B_WRITES (B_READS + 2 * (EDGES * RX_VECTORS + 8))
#define B_LLR (B_WRITES + 64)
#define B_HARD (B_LLR + 16224)
#define B_TOTAL (B_HARD + 2048)
#define P(type, at) ((type *)(base + (at)))
#define I(type, at) ((type *)(in + (at)))

static void run_pass1(int pie)
{
    (pie ? rx_pass1_pie : rx_pass1)(P(int8_t, B_BELIEF), P(uint16_t, B_READS), P(int8_t, B_R),
                                     P(int8_t, B_STATE), EDGES);
}
static void run_pass2(int pie)
{
    (pie ? rx_pass2_pie : rx_pass2)(P(int8_t, B_BELIEF), P(uint16_t, B_WRITES), P(int8_t, B_R),
                                     P(int8_t, B_SAVED), P(int8_t, B_STATE), 11 | 2 << 8);
}
static void run_compress(int pie)
{
    (pie ? rx_compress_pie : rx_compress)(P(int8_t, B_R), P(int8_t, B_STATE), P(int8_t, B_PACKED), EDGES);
}
static void run_expand(int pie)
{
    (pie ? rx_expand_pie : rx_expand)(P(int8_t, B_PACKED), P(int8_t, B_R), P(int8_t, B_SAVED), 11 | 2 << 8);
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
    (pie ? rx_xor_pie : rx_xor)(P(uint8_t, B_BELIEF), P(uint8_t, B_R), 91);
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
    for (unsigned i = 0; i < 16 * (EDGES * RX_VECTORS + 1); ++i) in[B_R + i] = (uint8_t)((int)(rnd() % 95) - 47);
    compare("pass1", run_pass1);
    /* pass 2 on what pass 1 left: 11 slots and 2 repeats into scratch slots. */
    for (unsigned j = 0; j < EDGES; ++j) I(uint16_t, B_WRITES)[j] = (uint16_t)(RX_SLOT * (3 * j + 1));
    rx_pass1(I(int8_t, B_BELIEF), I(uint16_t, B_READS), I(int8_t, B_R), I(int8_t, B_STATE), EDGES);
    compare("pass2", run_pass2);
    /* compress the messages pass 2 made (one argmin per check at most, as in a frame),
     * then expand them back. */
    rx_pass2(I(int8_t, B_BELIEF), I(uint16_t, B_WRITES), I(int8_t, B_R), I(int8_t, B_SAVED),
             I(int8_t, B_STATE), 11 | 2 << 8);
    compare("compress", run_compress);
    rx_compress(I(int8_t, B_R), I(int8_t, B_STATE), I(int8_t, B_PACKED), EDGES);
    memset(in + B_R, 0, 16 * (EDGES * RX_VECTORS + 1));
    compare("expand", run_expand);
    rx_expand(I(int8_t, B_PACKED), I(int8_t, B_R), I(int8_t, B_SAVED), 11 | 2 << 8);
    rx_pass1(I(int8_t, B_BELIEF), I(uint16_t, B_READS), I(int8_t, B_R), I(int8_t, B_STATE), EDGES);
    rx_pass2(I(int8_t, B_BELIEF), I(uint16_t, B_WRITES), I(int8_t, B_R), I(int8_t, B_SAVED),
             I(int8_t, B_STATE), 11 | 2 << 8);
    seed = 2;
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

/* Frame c: 0 clean; 1/2/3 one, 8 or 64 weak wrong bits; 4 100 confidently wrong bits;
 * 5/6/7 AWGN at 6.0/4.75/3.8 dB (3.8 dB runs all 8 iterations: the worst case); 8 random
 * LLRs (hopeless, stops at 3); 9 a 600-bit erasure. LLRs 32 per unit amplitude as the
 * demodulator scales them, hard bits their signs. */
static const char *const names[] = {"clean", "wrong_1", "wrong_8", "wrong_64", "flip_100",
                                    "awgn_6.0dB", "awgn_4.75dB", "awgn_3.8dB", "random", "erased"};
static void make_frame(frame_set_t *x, unsigned c)
{
    seed = 100 + c;
    const float esn0 = c == 5 ? 6.0f : c == 6 ? 4.75f : 3.8f, sigma = sqrtf(0.5f / powf(10, esn0 / 10));
    for (unsigned i = 0; i < RX_SHORT_BITS; ++i) {
        float y = x->bits[i] ? -1.0f : 1.0f;
        if (c >= 5 && c <= 7) y += 1.41421356f * sigma * gaussian();
        const float v = c == 8 ? (float)((int)(rnd() % 81) - 40) : roundf(32 * y);
        x->llr[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
    const unsigned wrong = c == 1 ? 1 : c == 2 ? 8 : c == 3 ? 64 : c == 4 ? 100 : 0;
    for (unsigned k = 0; k < wrong; ++k) {
        const unsigned i = rnd() % RX_SHORT_BITS;
        x->llr[i] = (int8_t)(c == 4 ? -x->llr[i] : x->bits[i] ? 4 : -4);
    }
    if (c == 9) memset(x->llr + 3000, 0, 600);
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
    /* Cold cases: after a write-back and invalidation of the whole data cache, and after
     * 16 KB of PSRAM writes, as core 1 writes LLRs each frame. */
    uint32_t flushed = UINT32_MAX, llr_writes = UINT32_MAX;
    for (unsigned run = 0; run < 2 * RUNS; ++run) {
        Cache_WriteBack_All();
        Cache_Invalidate_DCache_All();
        uint32_t t = decode(f, x, &length, &sum, &st);
        flushed = t < flushed ? t : flushed;
        for (unsigned i = 0; evictor && i < RX_SHORT_BITS; i += 4) *(volatile uint32_t *)(evictor + i) = i;
        t = decode(f, x, &length, &sum, &st);
        llr_writes = t < llr_writes ? t : llr_writes;
    }
    printf("@{\"cmd\":\"fecbench\",\"frame\":\"clean_cold\",\"flushed\":%" PRIu32 ",\"llr_writes\":%"
           PRIu32 ",\"payload_ok\":%s}\n", flushed, llr_writes, sum == x->want ? "true" : "false");
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
        printf(",\"length\":%d,\"payload_ok\":%s,\"iterations\":%u,\"failed_checks\":%u,"
               "\"unsatisfied\":%u}\n", length[1], sums[1] == x->want ? "true" : "false",
               st[1].iterations, st[1].failed_checks, st[1].unsatisfied);
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
    f->max_iterations = 8;
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
                /* The work area in PSRAM, the per-layer scratch internal (the default);
                 * then the scratch in PSRAM too; then the slots internal as well. */
                frames(f, x, "psram", false);
                int8_t *r = f->r, *saved = f->saved, *st = f->state, *b = f->belief;
                const size_t scratch = rx_fec_size(MODCOD) - (size_t)((uint8_t *)r - (uint8_t *)f);
                int8_t *ps = heap_caps_aligned_alloc(16, scratch, MALLOC_CAP_SPIRAM);
                if (ps) {
                    f->r = ps, f->saved = ps + (saved - r), f->state = ps + (st - r);
                    frames(f, x, "psram_scratch_psram", false);
                    f->r = r, f->saved = saved, f->state = st;
                }
                heap_caps_free(ps);
                const size_t slots = (size_t)(f->packed - b);
                memcpy(work, b, slots);
                f->belief = (int8_t *)work;
                frames(f, x, "psram_slots_internal", false);
                f->belief = b;
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

/* ---------------------------------------------------------------- curve */

/* Frames as the live receiver hands them to its decoder: a fresh GCS payload through the
 * transmitter's encoder, mapped as the receiver labels symbols (QPSK: bit 2k on I, 2k + 1 on Q;
 * 8PSK turned back by pi/8: b1 = x < 0, b2 = y < 0, b0 = |y| > |x|, bit i of symbol k from
 * column i of the interleaver, reversed for 3/5, as release_run reads them), complex Gaussian
 * noise at the given Es/N0, then the receiver's PIE demappers into hard, weak and (LEVELS 4)
 * fine bits. The decoder gets no soft bits, as on the board, and its memory is placed as in
 * receive.c: its state (rx_fec_size), work area and copy of the frame in PSRAM, its per-layer
 * scratch internal.
 * Symbols carry no carrier, timing or amplitude error: this is the decoder, not the
 * synchronization. One summary per call; cycles are rx_fec_frame's, check included, and
 * setup_cycles the first frame's check and belief load with no iteration allowed. */
#define CURVE_A 512                         /* constellation radius in symbol units */
#define CURVE_FRAMES 4000
#define LLR_UNIT 2965821                    /* demod.c: 32 sqrt(2) 2^16 */
#define LLR8_UNIT 5480192                   /* demod.c: 32 / sin(pi/8) 2^16 */

static uint32_t curve_cycles[CURVE_FRAMES];
static int16_t sym_i[RX_SHORT_BITS / 2] __attribute__((aligned(16)));
static int16_t sym_q[RX_SHORT_BITS / 2] __attribute__((aligned(16)));
static uint8_t no_scrambling[RX_SHORT_BITS / 8] __attribute__((aligned(16)));

static void gaussian2(float *a, float *b)
{
    const float u = ((rnd() >> 8) + 1) * (1.0f / 16777217.0f), v = (rnd() >> 8) * (1.0f / 16777216.0f);
    const float r = sqrtf(-2.0f * logf(u));
    *a = r * cosf(6.2831853f * v);
    *b = r * sinf(6.2831853f * v);
}

static unsigned bit(const uint8_t *c, unsigned i) { return c[i / 8] >> (7 - i % 8) & 1; }

static int16_t symbol16(float v)
{
    return (int16_t)lrintf(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

static int by_value(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void curve(unsigned modcod, int esn0_cdb, unsigned frames, unsigned levels)
{
    const dvbs2_mode *mode = dvbs2_find_mode_short(modcod);
    if (!mode) {
        printf("@{\"cmd\":\"curve\",\"error\":\"no short mode\"}\n");
        return;
    }
    const unsigned psk8 = modcod >= 12, bytes = (mode->kbch - 80) / 8;
    rx_fec_t *f = heap_caps_aligned_alloc(16, rx_fec_size(modcod), MALLOC_CAP_SPIRAM);
    void *work = heap_caps_aligned_alloc(16, rx_fec_work_size(modcod), MALLOC_CAP_SPIRAM);
    uint8_t *hard = heap_caps_aligned_alloc(16, RX_HARD_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *weak = heap_caps_aligned_alloc(16, RX_HARD_BYTES, MALLOC_CAP_SPIRAM), *payload = alloc(2048);
    uint8_t *fine = levels == 4 ? heap_caps_aligned_alloc(16, RX_HARD_BYTES, MALLOC_CAP_SPIRAM) : NULL;
    void *scratch = alloc(rx_fec_scratch_size(modcod));
    if (!f || !work || !hard || !weak || !payload || (levels == 4 && !fine) || !scratch ||
        rx_fec_init(f, modcod, work) || rx_fec_set_scratch(f, scratch)) {
        printf("@{\"cmd\":\"curve\",\"error\":\"setup\"}\n");
        goto out;
    }
    const float esn0 = esn0_cdb / 100.0f, sigma = CURVE_A / sqrtf(2.0f * powf(10.0f, esn0 / 10.0f));
    const float c8 = 0.92387953f * CURVE_A, s8 = 0.38268343f * CURVE_A, q = 0.70710678f * CURVE_A;
    demod_run_t k;
    demod_run_init(&k, 0, 0, 0, 0, 0, (int16_t)((psk8 ? LLR8_UNIT : LLR_UNIT) / 4 / CURVE_A));
    const unsigned n = psk8 ? RX_SHORT_BITS / 3 : RX_SHORT_BITS / 2, rev = modcod == 12;
    demod_out_t o = {{hard}, {weak}, {NULL}, {fine}};
    if (psk8) {
        for (unsigned p = 0; p < 3; ++p) {
            o.hard[p] = hard + (rev ? 2 - p : p) * (n / 8);
            o.weak[p] = weak + (rev ? 2 - p : p) * (n / 8);
            o.fine[p] = fine ? fine + (rev ? 2 - p : p) * (n / 8) : NULL;
        }
    }
    unsigned ok = 0, wrong = 0, failed = 0, hist[9] = {0};
    uint32_t setup = 0;
    uint64_t bit_errors = 0, cycles_sum = 0, by_iter[9] = {0};
    for (unsigned j = 0; j < frames; ++j) {
        seed = 0x9e3779b9u ^ modcod << 24 ^ (uint32_t)(esn0_cdb + 1000) << 12 ^ j;
        fill(payload, bytes);
        dvbs2_bb_gcs(&encoder_ws, mode, payload, mode->kbch - 80);
        dvbs2_bb_scramble(&encoder_ws, mode);
        dvbs2_bch(&encoder_ws, mode);
        dvbs2_ldpc(&encoder_ws, mode);
        const uint8_t *c = encoder_ws.bits;
        for (unsigned s = 0; s < n; ++s) {
            float x, y, ni, nq;
            if (psk8) {
                const unsigned b0 = bit(c, (rev ? 2 : 0) * n + s), b1 = bit(c, n + s), b2 = bit(c, (rev ? 0 : 2) * n + s);
                x = (b1 ? -1.0f : 1.0f) * (b0 ? s8 : c8);
                y = (b2 ? -1.0f : 1.0f) * (b0 ? c8 : s8);
            } else {
                x = bit(c, 2 * s) ? -q : q;
                y = bit(c, 2 * s + 1) ? -q : q;
            }
            gaussian2(&ni, &nq);
            sym_i[s] = symbol16(x + sigma * ni);
            sym_q[s] = symbol16(y + sigma * nq);
        }
        (psk8 ? (levels == 4 ? demod_8psk4_pie : demod_8psk_pie) :
                (levels == 4 ? demod_qpsk4_pie : demod_qpsk_pie))(sym_i, sym_q, n, no_scrambling, &k, &o);
        for (unsigned b = 0; b < RX_SHORT_BITS / 8; ++b) bit_errors += __builtin_popcount(hard[b] ^ c[b]);
        rx_frame_t frame = {.hard = hard, .weak = weak, .fine = fine, .modcod = (uint8_t)modcod, .type = 3};
        const uint8_t *p = NULL;
        rx_fec_stats_t st = {0};
        if (!j) {                   /* no iteration allowed: the check and the belief load alone */
            f->max_iterations = 0;
            const uint32_t t0 = now();
            rx_fec_frame(f, &frame, &p, &st);
            setup = now() - t0;
            f->max_iterations = 8;
        }
        const uint32_t t0 = now();
        const int length = rx_fec_frame(f, &frame, &p, &st);
        const uint32_t t = now() - t0;
        if (length < 0) ++failed;
        else if ((unsigned)length == bytes && crc(p, bytes) == crc(payload, bytes)) ++ok;
        else ++wrong;
        const unsigned it = st.iterations < 8 ? st.iterations : 8;
        ++hist[it];
        by_iter[it] += t;
        cycles_sum += t;
        curve_cycles[j] = t;
    }
    qsort(curve_cycles, frames, sizeof *curve_cycles, by_value);
    printf("@{\"cmd\":\"curve\",\"modcod\":%u,\"esn0_cdb\":%d,\"levels\":%u,\"frames\":%u,\"ok\":%u,\"wrong\":%u,"
           "\"failed\":%u,\"bit_errors\":%" PRIu64 ",\"bits\":%" PRIu64 ",\"cycles_mean\":%" PRIu64
           ",\"cycles_p50\":%" PRIu32 ",\"cycles_p90\":%" PRIu32 ",\"cycles_p99\":%" PRIu32
           ",\"cycles_max\":%" PRIu32 ",\"setup_cycles\":%" PRIu32 ",\"iterations\":[", modcod, esn0_cdb,
           levels, frames, ok, wrong, failed, bit_errors, (uint64_t)frames * RX_SHORT_BITS, cycles_sum / frames,
           curve_cycles[frames / 2], curve_cycles[frames * 9 / 10], curve_cycles[frames * 99 / 100],
           curve_cycles[frames - 1], setup);
    for (unsigned i = 0; i < 9; ++i) printf("%s%u", i ? "," : "", hist[i]);
    printf("],\"cycles_by_iterations\":[");
    for (unsigned i = 0; i < 9; ++i) printf("%s%" PRIu64, i ? "," : "", hist[i] ? by_iter[i] / hist[i] : 0);
    printf("]}\n");
out:
    heap_caps_free(f); heap_caps_free(work); heap_caps_free(hard); heap_caps_free(weak); heap_caps_free(fine);
    heap_caps_free(payload); heap_caps_free(scratch);
}

/* "setup MODCOD": what a decode costs before its first iteration, with the work area in PSRAM
 * (as on the board) and in internal RAM: the check alone (a decoder without a work area), the
 * check and belief load (no iteration allowed), and the two areas the load fills. Cold data
 * cache each time; the frame's bits are a fixed pattern. */
__attribute__((unused)) static void setup_costs(unsigned modcod)
{
    if (modcod < 4 || modcod == 11 || modcod > 16) {
        printf("@{\"cmd\":\"setup\",\"error\":\"setup MODCOD 4-10|12-16\"}\n");
        return;
    }
    /* Placements of the decoder's state, work area, frame copy and per-layer scratch: as
     * receive.c (PSRAM, scratch internal), all PSRAM (receive.c before 2026-10-25), the work area
     * alone in PSRAM, all internal. */
    static const char *const names[4] = {"live", "psram", "work_psram", "internal"};
    static const uint32_t caps[4][4] = {{MALLOC_CAP_SPIRAM, MALLOC_CAP_SPIRAM, MALLOC_CAP_SPIRAM, INTERNAL},
                                        {MALLOC_CAP_SPIRAM, MALLOC_CAP_SPIRAM, MALLOC_CAP_SPIRAM, 0},
                                        {INTERNAL, MALLOC_CAP_SPIRAM, INTERNAL, 0},
                                        {INTERNAL, INTERNAL, INTERNAL, 0}};
    const size_t size = rx_fec_size(modcod), work_size = rx_fec_work_size(modcod);
    rx_fec_t *check = (rx_fec_t *)alloc(size);
    uint8_t *frame_bits = alloc(2 * RX_HARD_BYTES);
    evictor = heap_caps_malloc(EVICT_BYTES, MALLOC_CAP_SPIRAM);
    uint32_t t[1 + 4 * 3] = {0};
    if (!check || !frame_bits || rx_fec_init(check, modcod, NULL)) {
        printf("@{\"cmd\":\"setup\",\"error\":\"allocation\"}\n");
        goto out;
    }
    const uint8_t *p;
    rx_fec_stats_t st;
    memset(frame_bits, 0x5a, RX_HARD_BYTES);
    memset(frame_bits + RX_HARD_BYTES, 0x21, RX_HARD_BYTES);
    rx_frame_t frame = {.hard = frame_bits, .weak = frame_bits + RX_HARD_BYTES, .modcod = (uint8_t)modcod, .type = 3};
    t[0] = now();
    rx_fec_frame(check, &frame, &p, &st);
    t[0] = now() - t[0];
    for (unsigned k = 0; k < 4; ++k) {
        rx_fec_t *f = heap_caps_aligned_alloc(16, size, caps[k][0]);
        void *work = heap_caps_aligned_alloc(16, work_size, caps[k][1]);
        uint8_t *bits = heap_caps_aligned_alloc(16, 2 * RX_HARD_BYTES, caps[k][2]);
        void *scratch = caps[k][3] ? heap_caps_aligned_alloc(16, rx_fec_scratch_size(modcod), caps[k][3]) : NULL;
        if (f && work && bits && !rx_fec_init(f, modcod, work) && (!caps[k][3] || !rx_fec_set_scratch(f, scratch))) {
            f->max_iterations = 0;
            memset(bits, 0x5a, RX_HARD_BYTES);
            memset(bits + RX_HARD_BYTES, 0x21, RX_HARD_BYTES);
            rx_frame_t x = {.hard = bits, .weak = bits + RX_HARD_BYTES, .modcod = (uint8_t)modcod, .type = 3};
            evict();
            uint32_t a = now();
            rx_fec_frame(f, &x, &p, &st);
            t[1 + 3 * k] = now() - a;
            evict();
            a = now();
            memset(f->packed, 0, f->packed_bytes);
            t[2 + 3 * k] = now() - a;
            evict();
            a = now();
            memset(f->belief, 0x11, RX_GROUPS * RX_SLOT);
            t[3 + 3 * k] = now() - a;
        }
        heap_caps_free(f); heap_caps_free(work); heap_caps_free(bits); heap_caps_free(scratch);
    }
    printf("@{\"cmd\":\"setup\",\"modcod\":%u,\"check\":%" PRIu32, modcod, t[0]);
    for (unsigned k = 0; k < 4; ++k) {
        printf(",\"%s\":{\"setup\":%" PRIu32 ",\"memset_packed\":%" PRIu32 ",\"memset_slots\":%" PRIu32 "}",
               names[k], t[1 + 3 * k], t[2 + 3 * k], t[3 + 3 * k]);
    }
    printf("}\n");
out:
    heap_caps_free(check); heap_caps_free(frame_bits); heap_caps_free((void *)evictor);
    evictor = NULL;
}

__attribute__((unused)) static bool curve_command(const char *line)
{
    unsigned modcod, frames, levels = 2;
    int cdb;
    if (sscanf(line, "curve %u %d %u %u", &modcod, &cdb, &frames, &levels) < 3) return false;
    if (modcod < 4 || modcod == 11 || modcod > 16 || cdb < -500 || cdb > 2000 || !frames ||
        frames > CURVE_FRAMES || (levels != 2 && levels != 4)) {
        return false;
    }
    curve(modcod, cdb, frames, levels);
    return true;
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
        else if (!strncmp(line, "setup", 5)) {
            setup_costs((unsigned)atoi(line + 5));
            printf("@{\"done\":true}\n");
        } else if (!strncmp(line, "curve", 5)) {
            if (!curve_command(line)) printf("@{\"error\":\"curve MODCOD 4-10|12-16 ESN0_CDB FRAMES 1-%u [LEVELS 2|4]\"}\n", CURVE_FRAMES);
            printf("@{\"done\":true}\n");
        } else printf("@{\"error\":\"commands: fecbench, curve MODCOD ESN0_CDB FRAMES [LEVELS], setup MODCOD\",\"done\":true}\n");
    }
}
#else
void app_main(void) { bench(); puts("QEMU-END"); }
#endif
