// Radio-off timings of the shipped shapers against portable C. Seven repetitions;
// minimum and median are cycles on this CPU, not an RF measurement. dvbpace adds
// the live encoder, ring, overlap copies and bank deadlines.
#include "membench.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "radio_s3.h"
#include "sdkconfig.h"
#include "shaper.h"
#include "shaper_qpsk.h"
#include "shaper_taps.h"
#define BANK_WORDS RADIO_S3_BANK_WORDS
#define REPEATS 7

static int by_value(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t checksum(const uint32_t *p, size_t words)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

// words > 0: report per sample and per equivalent 16380-word bank.
static void report(const char *test, uint32_t *cycles, unsigned words, uint32_t check)
{
    qsort(cycles, REPEATS, sizeof cycles[0], by_value);
    const uint32_t lo = cycles[0], mid = cycles[REPEATS / 2];
    printf("@{\"cmd\":\"membench\",\"test\":\"%s\",\"cycles_min\":%lu,\"cycles_median\":%lu",
           test, (unsigned long)lo, (unsigned long)mid);
    if (words) {
        const double per_word = (double)lo / words;
        printf(",\"words\":%u,\"cycles_per_word\":%.3f,\"us_per_bank\":%.1f", words, per_word,
               per_word * BANK_WORDS / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }
    printf(",\"check\":%lu}\n", (unsigned long)check);
}

#define TIME(cycles, statement)                                    \
    for (int r_ = 0; r_ < REPEATS; ++r_) {                         \
        const uint32_t t0_ = esp_cpu_get_cycle_count();            \
        statement;                                                 \
        (cycles)[r_] = esp_cpu_get_cycle_count() - t0_;            \
    }

static void shaper_bench(void)
{
    static const struct {
        const char *name;
        const int32_t *taps;
        int n, bits, nsym;
    } cases[] = {
        {"shape4a35", shaper_taps_8psk_n4_a35, 4, 3, 4093},
        {"shape8a35", shaper_taps_8psk_n8_a35, 8, 3, 2047},
        {"shape40a35", shaper_taps_qpsk_n40_a35, 40, 2, 409},
        {"qpsk8", shaper_taps_8psk_n8_a35, 8, 2, 2047},
        {"qpsk4", shaper_taps_8psk_n4_a35, 4, 2, 4093},
    };
    uint8_t *sym = heap_caps_malloc(SHAPER_HISTORY + 4095, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sym) { printf("@{\"cmd\":\"membench\",\"error\":\"allocation failed\"}\n"); return; }
    uint32_t *bank = radio_s3_bank(0), cycles[REPEATS];
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const int n = cases[c].n, bits = cases[c].bits, nsym = cases[c].nsym;
        shaper_table_t t;
        char name[24];
        TIME(cycles, shaper_build(&t, shaper_store, cases[c].taps, n, bits));
        snprintf(name, sizeof name, "%s_build", cases[c].name);
        report(name, cycles, 0, checksum(shaper_store, shaper_table_words(n,bits)));
        uint32_t x = 2026;
        for (int i = 0; i < SHAPER_HISTORY + nsym; ++i) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            sym[i] = 16 * (bits == 3 ? (x >> 16) & 7 : 2 * ((x >> 16) & 3) + 1);
        }
        TIME(cycles, shaper_fill_c(bank, sym + SHAPER_HISTORY, nsym, &t));
        snprintf(name, sizeof name, "%s_c", cases[c].name);
        report(name, cycles, nsym*n, checksum(bank, nsym*n));
        if (bits == 2 && n != 40)
            for (int i = 0; i < SHAPER_HISTORY + nsym; ++i) sym[i] = (sym[i] >> 5) << 4;
        TIME(cycles, shaper_samples_codes_profile(bank, sym + SHAPER_HISTORY, 0, nsym*n, &t, NULL, NULL));
        snprintf(name, sizeof name, "%s_pie", cases[c].name);
        report(name, cycles, nsym*n, checksum(bank, nsym*n));
    }
    heap_caps_free(sym);
}

static void short_bench(void)
{
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint8_t *sym = heap_caps_malloc(4104, internal);
    if (!sym) { printf("@{\"cmd\":\"membench\",\"error\":\"short allocation failed\"}\n"); return; }
    uint32_t x = 2026, cycles[REPEATS], *bank = radio_s3_bank(0);
    for (unsigned i = 0; i < 4104; ++i) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        sym[i] = ((x >> 16) & 7) << 4;
    }
    shaper_table_t t;
    shaper_short_build(&t, shaper_store, 128);
    TIME(cycles, shaper_fill_c(bank, sym+8, 4095, &t));
    report("short4_c", cycles, 16380, checksum(bank,16380));
    TIME(cycles, shaper_short4_pie(bank, sym+8, 4095, t.blocks));
    report("short4_pie", cycles, 16380, checksum(bank,16380));
    heap_caps_free(sym);
}

static void qpsk_bench(void)
{
    uint8_t *src = (uint8_t *)radio_s3_bank(1);
    uint32_t *dst = radio_s3_bank(0), cycles[REPEATS];
    uint32_t x = 2026;
    for (unsigned i = 0; i < 2+BANK_WORDS/12; ++i) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        src[i] = x >> 16;
    }
    for (unsigned n = 3; n <= 4; ++n) {
        char name[24];
        const unsigned groups = BANK_WORDS/(4*n), words = groups*4*n;
        shaper_qpsk_build(shaper_store,n,128);
        snprintf(name,sizeof name,"qpsk%u_c",n);
        TIME(cycles,shaper_qpsk_c(dst,src+2,groups,shaper_store,n));
        report(name,cycles,words,checksum(dst,words));
        snprintf(name,sizeof name,"qpsk%u_pie",n);
        TIME(cycles,(n == 3 ? shaper_qpsk3_pie : shaper_qpsk4_pie)(dst,src+2,groups,shaper_store));
        report(name,cycles,words,checksum(dst,words));
    }
}

void membench_run(void)
{
    shaper_bench();
    short_bench();
    qpsk_bench();
}
