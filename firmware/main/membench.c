// Memory timing for the transmit path.
//
// The radio's DAC engine loops over one bank of 16380 32-bit words (one complex sample each) at
// 40 MS/s, 409.5 us per pass, while the CPU writes the next samples into the other bank. At 8
// samples per symbol (5 Msym/s) one symbol is one 8-word table row, so filling a bank is mostly
// copying rows. These tests time that copy and the PSRAM traffic around it. Kernels run from
// IRAM so instruction fetch does not depend on the flash cache. Each test runs REPEATS times;
// the minimum is the cleanest run, the median shows interrupt and cache effects.

#include "membench.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#define BANK_WORDS 16380                  // one DAC bank
#define ROW_WORDS 8                       // samples per symbol at 5 Msym/s, 40 MS/s
#define SYMBOLS (BANK_WORDS / ROW_WORDS)  // 2047 rows = 16376 words
#define ROWS 1024                         // 32 KiB table of 8-word rows
#define BIG (1u << 20)                    // PSRAM test size, bytes
#define REPEATS 7
#define CPU_HZ (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000.0)

static IRAM_ATTR __attribute__((noinline)) void fill_store(uint32_t *bank, uint32_t x)
{
    for (int i = 0; i < BANK_WORDS; i += 4) {  // 16380 = 4 x 4095
        bank[i] = x;
        bank[i + 1] = x + 1;
        bank[i + 2] = x + 2;
        bank[i + 3] = x + 3;
        x += 4;
    }
}

static IRAM_ATTR __attribute__((noinline)) void fill_rows(uint32_t *bank, const uint32_t *table,
                                                          const uint16_t *index)
{
    for (int s = 0; s < SYMBOLS; ++s) {
        const uint32_t *r = table + index[s] * ROW_WORDS;
        uint32_t *o = bank + s * ROW_WORDS;
        o[0] = r[0], o[1] = r[1], o[2] = r[2], o[3] = r[3];
        o[4] = r[4], o[5] = r[5], o[6] = r[6], o[7] = r[7];
    }
}

// I and Q come from separate per-axis tables whose rows occupy different bit fields of the DAC
// word, so one OR combines them.
static IRAM_ATTR __attribute__((noinline)) void fill_rows_or(uint32_t *bank, const uint32_t *ti,
                                                             const uint32_t *tq, const uint16_t *ii,
                                                             const uint16_t *iq)
{
    for (int s = 0; s < SYMBOLS; ++s) {
        const uint32_t *a = ti + ii[s] * ROW_WORDS, *b = tq + iq[s] * ROW_WORDS;
        uint32_t *o = bank + s * ROW_WORDS;
        o[0] = a[0] | b[0], o[1] = a[1] | b[1], o[2] = a[2] | b[2], o[3] = a[3] | b[3];
        o[4] = a[4] | b[4], o[5] = a[5] | b[5], o[6] = a[6] | b[6], o[7] = a[7] | b[7];
    }
}

static IRAM_ATTR __attribute__((noinline)) uint32_t read_sum(const uint32_t *p, size_t words)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    for (size_t i = 0; i < words; i += 4) {
        a += p[i], b += p[i + 1], c += p[i + 2], d += p[i + 3];
    }
    return a ^ b ^ c ^ d;
}

static IRAM_ATTR __attribute__((noinline)) void write_words(uint32_t *p, size_t words, uint32_t x)
{
    for (size_t i = 0; i < words; i += 4) {
        p[i] = x, p[i + 1] = x, p[i + 2] = x, p[i + 3] = x;
    }
}

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

// words > 0: a bank test, reported per word and per 16380-word bank. bytes > 0: a bandwidth test.
static void report(const char *test, uint32_t *cycles, unsigned words, unsigned bytes, uint32_t check)
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
    if (bytes) {
        printf(",\"bytes\":%u,\"mb_per_s\":%.1f", bytes, bytes / (lo / CPU_HZ) / 1e6);
    }
    printf(",\"check\":%lu}\n", (unsigned long)check);
}

#define TIME(cycles, statement)                                    \
    for (int r_ = 0; r_ < REPEATS; ++r_) {                         \
        const uint32_t t0_ = esp_cpu_get_cycle_count();            \
        statement;                                                 \
        (cycles)[r_] = esp_cpu_get_cycle_count() - t0_;            \
    }

void membench_run(void)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint32_t *bank = heap_caps_aligned_alloc(16, BANK_WORDS * 4, internal);
    uint32_t *ti = heap_caps_aligned_alloc(16, ROWS * ROW_WORDS * 4, internal);
    uint32_t *tq = heap_caps_aligned_alloc(16, ROWS * ROW_WORDS * 4, internal);
    uint16_t *ii = heap_caps_malloc(SYMBOLS * 2, internal);
    uint16_t *iq = heap_caps_malloc(SYMBOLS * 2, internal);
    uint32_t *chunk = heap_caps_aligned_alloc(16, 65536, internal);
    uint32_t *big = heap_caps_aligned_alloc(16, BIG, MALLOC_CAP_SPIRAM);
    if (!bank || !ti || !tq || !ii || !iq || !chunk) {
        printf("@{\"cmd\":\"membench\",\"error\":\"internal allocation failed\"}\n");
        goto done;
    }

    // Tables and row indices: fixed pseudo-random contents (xorshift32), so runs are comparable.
    uint32_t x = 2026;
    for (int i = 0; i < ROWS * ROW_WORDS; ++i) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        ti[i] = x & 0x000003FFu;  // TX I field, bits 9:0 (RX uses the opposite order)
        tq[i] = x & 0x000FFC00u;  // TX Q field, bits 19:10
    }
    for (int s = 0; s < SYMBOLS; ++s) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        ii[s] = x % ROWS;
        iq[s] = (x >> 16) % ROWS;
    }

    uint32_t cycles[REPEATS];
    TIME(cycles, fill_store(bank, 7));
    report("bank_store", cycles, BANK_WORDS, 0, checksum(bank, BANK_WORDS));
    TIME(cycles, fill_rows(bank, ti, ii));
    report("bank_rows", cycles, SYMBOLS * ROW_WORDS, 0, checksum(bank, SYMBOLS * ROW_WORDS));
    TIME(cycles, fill_rows_or(bank, ti, tq, ii, iq));
    report("bank_rows_or", cycles, SYMBOLS * ROW_WORDS, 0, checksum(bank, SYMBOLS * ROW_WORDS));
    TIME(cycles, memcpy(chunk, bank, 65520));
    report("sram_memcpy", cycles, 0, 65520, checksum(chunk, 16380));

    if (!big) {
        printf("@{\"cmd\":\"membench\",\"error\":\"PSRAM allocation failed\"}\n");
        goto done;
    }
    uint32_t sum = 0;
    TIME(cycles, write_words(big, BIG / 4, 0x5A5A5A5Au));
    report("psram_write", cycles, 0, BIG, checksum(big, 1024));
    TIME(cycles, sum ^= read_sum(big, BIG / 4));
    report("psram_read", cycles, 0, BIG, sum);
    TIME(cycles, for (unsigned k = 0; k < BIG; k += 65536) memcpy(chunk, (uint8_t *)big + k, 65536));
    report("psram_to_sram", cycles, 0, BIG, checksum(chunk, 16384));

done:
    heap_caps_free(bank), heap_caps_free(ti), heap_caps_free(tq), heap_caps_free(ii);
    heap_caps_free(iq), heap_caps_free(chunk), heap_caps_free(big);
}
