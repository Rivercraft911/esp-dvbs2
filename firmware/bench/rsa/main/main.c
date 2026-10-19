// Ordinary integer multiplier only. No Wi-Fi, PHY, DAC, NVS or key APIs.
#include "poly.h"
#include "dvbs2.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "esp_crypto_periph_clk.h"
#include "hal/mpi_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TRIALS 16
#define TIMEOUT 24000000U
static uint32_t x[64], y[64], z[128], a[8], b[8], got[16], expected[16];
static uint32_t random_state;

static uint32_t rng(void)
{
    uint32_t v = random_state;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return random_state = v;
}

static void metric(const char *name, uint32_t *v)
{
    for (unsigned i = 1; i < TRIALS; ++i) {
        for (unsigned j = i; j && v[j] < v[j - 1]; --j) {
            uint32_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    }
    printf("\"%s\":{\"min\":%"PRIu32",\"median\":%"PRIu32",\"max\":%"PRIu32"}",
           name, v[0], v[TRIALS / 2], v[TRIALS - 1]);
}

static bool wait_clean(void)
{
    uint32_t start = esp_cpu_get_cycle_count();
    // IDF v6.1's LL helper returns true while CLEAN is zero, as does its HAL loop.
    while (mpi_ll_check_memory_init_complete()) {
        if ((uint32_t)(esp_cpu_get_cycle_count() - start) > TIMEOUT) return false;
    }
    return true;
}

static bool multiply(unsigned words)
{
    // IDF bignum_alt.c puts Y in the upper Z half. Clear the lower half on
    // warm reuse; IDF normally gets those zeros from hardware acquisition.
    mpi_ll_write_to_mem_block(MPI_PARAM_X, 0, x, words, words);
    mpi_ll_write_to_mem_block(MPI_PARAM_Z, 0, NULL, 0, words);
    mpi_ll_write_to_mem_block(MPI_PARAM_Z, words * 4, y, words, words);
    mpi_ll_set_mode(2 * words - 1);
    mpi_ll_clear_interrupt();
    mpi_ll_start_op(MPI_MULT);
    uint32_t start = esp_cpu_get_cycle_count();
    // The LL status helper likewise returns true until the interrupt becomes one.
    while (mpi_ll_get_int_status()) {
        if ((uint32_t)(esp_cpu_get_cycle_count() - start) > TIMEOUT) return false;
    }
    mpi_ll_read_from_mem_block(z, 2 * words, 2 * words);
    mpi_ll_clear_interrupt();
    return true;
}

static bool product_case(unsigned bits, unsigned spacing, bool fixed)
{
    uint32_t pack[TRIALS], hardware[TRIALS], extract[TRIALS], total[TRIALS];
    unsigned words = (bits * spacing + 31) / 32;
    unsigned n = (bits + 31) / 32, out = (2 * bits - 1 + 31) / 32;
    random_state = 2026;
    for (unsigned j = 0; j < n; ++j) b[j] = rng();
    if (bits % 32) b[n - 1] &= (1U << (bits % 32)) - 1;
    poly_pack(y, b, bits, spacing);
    for (unsigned i = 0; i < TRIALS; ++i) {
        for (unsigned j = 0; j < n; ++j) {
            a[j] = i == 0 ? 0 : i == 1 ? UINT32_MAX : rng();
            if (!fixed) b[j] = i < 2 ? UINT32_MAX : rng();
        }
        if (bits % 32) {
            a[n - 1] &= (1U << (bits % 32)) - 1;
            b[n - 1] &= (1U << (bits % 32)) - 1;
        }
        poly_reference(expected, a, b, bits);
        uint32_t t0 = esp_cpu_get_cycle_count();
        poly_pack(x, a, bits, spacing);
        if (!fixed) poly_pack(y, b, bits, spacing);
        uint32_t t1 = esp_cpu_get_cycle_count();
        if (!multiply(words)) {
            printf("@{\"cmd\":\"rsa_bench\",\"error\":\"multiply timeout\",\"bits\":%u}\n", bits);
            return false;
        }
        uint32_t t2 = esp_cpu_get_cycle_count();
        poly_extract(got, z, 2 * bits - 1, spacing);
        uint32_t t3 = esp_cpu_get_cycle_count();
        if (memcmp(got, expected, out * 4)) {
            printf("@{\"cmd\":\"rsa_bench\",\"error\":\"product mismatch\",\"bits\":%u,\"trial\":%u}\n", bits, i);
            return false;
        }
        pack[i] = t1 - t0;
        hardware[i] = t2 - t1;
        extract[i] = t3 - t2;
        total[i] = t3 - t0;
        vTaskDelay(1);
    }
    printf("@{\"cmd\":\"rsa_product\",\"bits\":%u,\"spacing\":%u,\"hardware_words\":%u,"
           "\"fixed_rhs\":%s,\"trials\":%u,\"exact_products\":%u,\"engine\":\"warm\",",
           bits, spacing, words, fixed ? "true" : "false", TRIALS, TRIALS);
    metric("pack_cycles", pack);
    printf(",");
    metric("mmio_launch_wait_read_cycles", hardware);
    printf(",");
    metric("extract_cycles", extract);
    printf(",");
    metric("total_cycles", total);
    printf("}\n");
    return true;
}

static bool bch_case(unsigned mc)
{
    const dvbs2_mode *mode = dvbs2_find_mode(mc);
    dvbs2_fast *f = heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    dvbs2_workspace *ws = heap_caps_aligned_alloc(16, sizeof *ws, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    dvbs2_workspace *reference = heap_caps_aligned_alloc(16, sizeof *reference, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *cache = NULL, *pairs = NULL;
    bool ok = false;
    uint32_t cycles[TRIALS];
    if (!f || !ws || !reference || dvbs2_fast_prepare(f, mode, 1, 15)) goto done;
    size_t bytes = dvbs2_fast_cache_size(f);
    cache = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!cache || dvbs2_fast_cache_tables(f, cache)) goto done;
    if (mode->t != 8) {
        pairs = heap_caps_aligned_alloc(16, DVBS2_BCH_PAIR_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!pairs || dvbs2_fast_cache_bch_pair(f, pairs)) goto done;
    }
    random_state = 2026;
    for (unsigned i = 0; i < mode->kbch / 8; ++i) ws->bits[i] = rng();
    memcpy(reference->bits, ws->bits, mode->kbch / 8);
    dvbs2_bch(reference, mode);
    dvbs2_fast_bch(f, ws);
    for (unsigned i = 0; i < TRIALS; ++i) {
        uint32_t start = esp_cpu_get_cycle_count();
        dvbs2_fast_bch(f, ws);
        cycles[i] = esp_cpu_get_cycle_count() - start;
        if (memcmp(ws->bits, reference->bits, mode->nbch / 8)) goto done;
        vTaskDelay(1);
    }
    printf("@{\"cmd\":\"bch_baseline\",\"modcod\":%u,\"kbch\":%u,\"parity_bits\":%u,"
           "\"trials\":%u,\"exact_frames\":%u,\"variant\":\"%s\",",
           mc, mode->kbch, mode->nbch - mode->kbch, TRIALS, TRIALS,
           pairs ? "resident-split-pair" : "resident-pair");
    metric("cycles", cycles);
    printf("}\n");
    ok = true;
 done:
    heap_caps_free(pairs);
    heap_caps_free(cache);
    heap_caps_free(reference);
    heap_caps_free(ws);
    heap_caps_free(f);
    if (!ok) printf("@{\"cmd\":\"rsa_bench\",\"error\":\"BCH baseline failure\",\"modcod\":%u}\n", mc);
    return ok;
}

static void bench(void)
{
    if (esp_clk_cpu_freq() != 240000000) {
        printf("@{\"cmd\":\"rsa_bench\",\"error\":\"need 240MHz CPU\"}\n");
        return;
    }
    uint32_t start = esp_cpu_get_cycle_count();
    // SDK ordinary RSA enable releases shared DS reset; no DS operation occurs.
    esp_crypto_mpi_enable_periph_clk(true);
    bool ok = wait_clean();
    mpi_ll_disable_interrupt();
    printf("@{\"cmd\":\"rsa_init\",\"radio_off\":true,\"cpu_hz\":240000000,"
           "\"cycles\":%"PRIu32",\"ready\":%s}\n",
           esp_cpu_get_cycle_count() - start, ok ? "true" : "false");
    if (ok) {
        ok = product_case(64, 7, false) && product_case(64, 7, true) &&
             product_case(248, 8, false) && product_case(248, 8, true);
    }
    esp_crypto_mpi_enable_periph_clk(false);
    if (ok) ok = bch_case(7) && bch_case(13) && bch_case(17);
    printf("@{\"cmd\":\"rsa_bench\",\"passed\":%s,\"radio_off\":true,\"timeout_cycles\":%u}\n",
           ok ? "true" : "false", TIMEOUT);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char line[64];
    unsigned used = 0;
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (used + 1 < sizeof line) line[used++] = (char)c;
            continue;
        }
        line[used] = 0;
        used = 0;
        if (!strcmp(line, "rsa_bench")) bench();
        else if (!strcmp(line, "info")) {
            printf("@{\"cmd\":\"info\",\"firmware\":\"rsa-bench-02\",\"radio_off\":true,\"cpu_hz\":%d}\n",
                   esp_clk_cpu_freq());
        } else printf("@{\"error\":\"use info or rsa_bench\"}\n");
        printf("@{\"done\":true}\n");
    }
}
