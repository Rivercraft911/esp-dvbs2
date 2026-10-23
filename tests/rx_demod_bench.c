// Demodulator kernels on the board, radio off: each PIE kernel against its portable C version
// (demod.c) on random inputs, same bytes required, both timed. Results are @JSON lines ending
// with {"done":true}; the bench reruns every few seconds.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "demod_pie.h"
#include "demod_tables.h"

#define N 1440              /* one data run between pilot blocks */
#define RUNS 5
#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static uint32_t seed = 2026;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static uint32_t now(void) { return esp_cpu_get_cycle_count(); }

static void report(const char *name, unsigned n, uint32_t c, uint32_t pie, unsigned diff)
{
    printf("@{\"kernel\":\"%s\",\"symbols\":%u,\"c_cycles\":%" PRIu32 ",\"pie_cycles\":%" PRIu32
           ",\"pie_per_symbol\":%.2f,\"same\":%s,\"diff\":%u}\n", name, n, c, pie,
           (double)pie / n, diff ? "false" : "true", diff);
}

/* The matched filter over a random ring: 768 outputs (one capture unit) from a random time. */
static void bench_mf(void)
{
    int8_t *ring = heap_caps_aligned_alloc(16, 2 * DEMOD_STRIDE, INTERNAL);
    int16_t *o[4];
    for (int i = 0; i < 4; ++i) o[i] = heap_caps_aligned_alloc(16, 2 * 768, INTERNAL);
    if (!ring || !o[0] || !o[1] || !o[2] || !o[3]) {
        printf("@{\"error\":\"allocation failed\"}\n");
        return;
    }
    for (unsigned i = 0; i < 2 * DEMOD_STRIDE; ++i) ring[i] = (int8_t)rnd();
    for (unsigned rep = 0; rep < 3; ++rep) {
        uint64_t t = ((uint64_t)rnd() << 32 | rnd()) & 0xffffffffffull;
        demod_mf_t m0 = {ring, ring + DEMOD_STRIDE, rx_mf_sets, (uint32_t)t, (uint32_t)(t >> 32),
                         (16u << 24) + rnd() % 4096 - 2048, 0, 0};
        uint32_t best[2] = {UINT32_MAX, UINT32_MAX};
        demod_mf_t out[2];
        for (int pie = 0; pie < 2; ++pie) {
            for (unsigned r = 0; r < RUNS; ++r) {
                demod_mf_t m = m0;
                uint32_t c = now();
                (pie ? demod_mf_pie : demod_mf_c)(&m, 768, o[2 * pie], o[2 * pie + 1]);
                c = now() - c;
                best[pie] = c < best[pie] ? c : best[pie];
                out[pie] = m;
            }
        }
        unsigned diff = memcmp(o[0], o[2], 2 * 768) != 0 || memcmp(o[1], o[3], 2 * 768) != 0 ||
                        memcmp(&out[0], &out[1], sizeof out[0]) != 0;
        report("demod_mf", 768, best[0], best[1], diff);
    }
    for (int i = 0; i < 4; ++i) heap_caps_free(o[i]);
    heap_caps_free(ring);
}

/* The search's integrate-and-dump over 512 random samples per axis. */
static void bench_halves(void)
{
    static const int8_t w[32] __attribute__((aligned(16))) = {
        1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    int8_t *x = heap_caps_aligned_alloc(16, 1024, INTERNAL);
    int16_t *h = heap_caps_aligned_alloc(16, 4 * 128, INTERNAL);
    if (!x || !h) return;
    for (unsigned i = 0; i < 1024; ++i) x[i] = (int8_t)rnd();
    uint32_t best[2] = {UINT32_MAX, UINT32_MAX};
    for (int pie = 0; pie < 2; ++pie) {
        for (unsigned r = 0; r < RUNS; ++r) {
            uint32_t c = now();
            (pie ? demod_halves_pie : demod_halves_c)(x, x + 512, 64, h + 128 * pie, h + 128 * pie + 64, w);
            c = now() - c;
            best[pie] = c < best[pie] ? c : best[pie];
        }
    }
    report("demod_halves", 64, best[0], best[1], memcmp(h, h + 128, 256) != 0);
    heap_caps_free(x);
    heap_caps_free(h);
}

/* The search's signs over random half sums, including the extremes, then the a_{k-2} carry. */
static void bench_signs(void)
{
    int16_t *h = heap_caps_aligned_alloc(16, 4 * 72, INTERNAL);
    int16_t *a = heap_caps_aligned_alloc(16, 4 * 16, INTERNAL);
    int16_t *scratch = heap_caps_aligned_alloc(16, 512, INTERNAL);
    if (!h || !a || !scratch) {
        printf("@{\"error\":\"allocation failed\"}\n");
        return;
    }
    for (unsigned rep = 0; rep < 4; ++rep) {
        for (unsigned i = 0; i < 2 * 72; ++i) {
            int32_t v = (int32_t)(rnd() % 1021) - 512;          /* sums of 4 int8: -512 .. 508 */
            h[i] = (int16_t)(rep == 3 ? ((rnd() & 1) ? 508 : -512) : rep == 2 ? v / 64 : v);
        }
        for (unsigned i = 0; i < 16; ++i) a[i] = (int16_t)((int32_t)(rnd() % 2041) - 1024);
        memcpy(a + 16, a, 32);
        int32_t sums[4] = {5, -7, 5, -7};
        uint64_t bits[2] = {0, 0};
        uint32_t best[2] = {UINT32_MAX, UINT32_MAX};
        for (int pie = 0; pie < 2; ++pie) {
            for (unsigned r = 0; r < RUNS; ++r) {
                int16_t carry[16] __attribute__((aligned(16)));
                memcpy(carry, a + 16 * pie, 32);
                int32_t s[2] = {5, -7};
                uint32_t c = now();
                bits[pie] = (pie ? demod_signs_pie : demod_signs_c)(h, h + 72, carry, s, scratch);
                c = now() - c;
                best[pie] = c < best[pie] ? c : best[pie];
                if (r == RUNS - 1) {
                    memcpy(a + 16 * pie, carry, 32);
                    sums[2 * pie] = s[0];
                    sums[2 * pie + 1] = s[1];
                }
            }
        }
        unsigned diff = (bits[0] != bits[1]) + (memcmp(a, a + 16, 32) != 0) +
                        (sums[0] != sums[2]) + (sums[1] != sums[3]);
        report("demod_signs", 64, best[0], best[1], diff);
    }
    heap_caps_free(h);
    heap_caps_free(a);
    heap_caps_free(scratch);
}

static void bench(void)
{
    int16_t *yi = heap_caps_aligned_alloc(16, 2 * N + 32, INTERNAL), *yq = heap_caps_aligned_alloc(16, 2 * N + 32, INTERNAL);
    int16_t *xi = heap_caps_aligned_alloc(16, 2 * N + 32, INTERNAL), *xq = heap_caps_aligned_alloc(16, 2 * N + 32, INTERNAL);
    int16_t *ci = heap_caps_aligned_alloc(16, 2 * N + 32, INTERNAL), *cq = heap_caps_aligned_alloc(16, 2 * N + 32, INTERNAL);
    int8_t *llr[2] = {heap_caps_aligned_alloc(16, 2 * N + 32, MALLOC_CAP_SPIRAM),
                      heap_caps_aligned_alloc(16, 2 * N + 32, MALLOC_CAP_SPIRAM)};
    uint8_t *hard[2] = {heap_caps_aligned_alloc(16, N / 4 + 16, INTERNAL), heap_caps_aligned_alloc(16, N / 4 + 16, INTERNAL)};
    uint8_t *scr = heap_caps_aligned_alloc(16, N / 4 + 16, INTERNAL);
    demod_run_t *k = heap_caps_aligned_alloc(16, sizeof *k, INTERNAL);
    if (!yi || !yq || !xi || !xq || !ci || !cq || !llr[0] || !llr[1] || !hard[0] || !hard[1] || !scr || !k) {
        printf("@{\"error\":\"allocation failed\"}\n");
        return;
    }
    for (unsigned rep = 0; rep < 3; ++rep) {
        for (unsigned i = 0; i < N + 16; ++i) {
            yi[i] = (int16_t)((int32_t)(rnd() % 24001) - 12000);
            yq[i] = (int16_t)((int32_t)(rnd() % 24001) - 12000);
        }
        if (rep == 2) {                     /* extremes: saturation corners */
            for (unsigned i = 0; i < N + 16; i += 3) {
                yi[i] = (rnd() & 1) ? 32767 : -32768;
                yq[i] = (rnd() & 1) ? 32767 : -32768;
            }
        }
        for (unsigned i = 0; i < N / 4 + 16; ++i) scr[i] = (uint8_t)rnd();
        uint32_t inc = rnd(), phase = rnd();
        demod_run_init(k, inc, (int16_t)(rnd() % 801) - 400, (int16_t)(rnd() % 801) - 400,
                       (int16_t)(rnd() % 201) - 100, (int16_t)(rnd() % 201) - 100,
                       (int16_t)(rep == 2 ? 8148 : 60 + rnd() % 400));
        uint32_t best[2] = {UINT32_MAX, UINT32_MAX};
        for (int pie = 0; pie < 2; ++pie) {
            for (unsigned r = 0; r < RUNS; ++r) {
                memcpy(pie ? xi : ci, yi, 2 * N + 32);
                memcpy(pie ? xq : cq, yq, 2 * N + 32);
                uint32_t t = now();
                (pie ? demod_turn_pie : demod_turn_c)(pie ? xi : ci, pie ? xq : cq, N / 8, phase, 8 * inc, k);
                t = now() - t;
                best[pie] = t < best[pie] ? t : best[pie];
            }
        }
        unsigned diff = 0;
        for (unsigned i = 0; i < N; ++i) diff += (xi[i] != ci[i]) + (xq[i] != cq[i]);
        report("demod_turn", N, best[0], best[1], diff);
        for (unsigned n = N; n >= 900; n -= 540) {
            best[0] = best[1] = UINT32_MAX;
            for (int pie = 0; pie < 2; ++pie) {
                for (unsigned r = 0; r < RUNS; ++r) {
                    memset(llr[pie], 0x55, 2 * N + 32);
                    memset(hard[pie], 0x55, N / 4 + 16);
                    demod_out_t o = {llr[pie], hard[pie]};
                    uint32_t t = now();
                    (pie ? demod_llr_pie : demod_llr_c)(ci, cq, n, scr, k, &o);
                    t = now() - t;
                    best[pie] = t < best[pie] ? t : best[pie];
                }
            }
            diff = 0;
            for (unsigned i = 0; i < 2 * N + 32; ++i) diff += llr[0][i] != llr[1][i];
            for (unsigned i = 0; i < N / 4 + 16; ++i) diff += hard[0][i] != hard[1][i];
            report(n == N ? "demod_llr" : "demod_llr_tail", n, best[0], best[1], diff);
        }
    }
    printf("@{\"done\":true}\n");
}

void app_main(void)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        bench_mf();
        bench_halves();
        bench_signs();
        bench();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
