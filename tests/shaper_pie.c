// ESP-IDF/QEMU harness for the n4/n5 PIE kernels and the raw-phase glue. No radio or board access.
// Build with shaper.c and shaper_pie.S, then pass the UART log to tests/test_shaper.py.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "shaper.h"
#include "shaper_taps.h"

// Reuse the bank-sized oracle buffers for both suites so the QEMU app fits SRAM.
extern uint32_t test_reference[16440], test_guarded[4 + 16384 + 4];
extern uint8_t test_symbols[SHAPER_HISTORY + 4100];
#define blocks shaper_store
#define reference test_reference
#define guarded test_guarded
#define symbols test_symbols
static const uint32_t *rows[7 + 4095];

static uint32_t checksum(const uint32_t *p, size_t words)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

int tx_samples_check(void);
int maxrate_check(void);

void app_main(void)
{
    static const int counts4[] = {0, 1, 2, 3, 4, 5, 6, 7, 13, 4093, 4094, 4095};
    static const int counts5[] = {0, 4, 8, 12, 16, 3276};
    static const uint32_t seeds[] = {2026, 7, UINT32_MAX};
    static const int32_t *taps[] = {shaper_taps_8psk_n4_a20, shaper_taps_8psk_n4_a35,
                                    shaper_taps_8psk_n5_a20, shaper_taps_8psk_n5_a35};
    static const char *names[] = {"8psk-n4-a20", "8psk-n4-a35", "8psk-n5-a20", "8psk-n5-a35"};
    puts("QEMU-START");
    for (int profile = 0; profile < 4; ++profile) {
        shaper_table_t t;
        const int n = profile < 2 ? 4 : 5;
        const int *counts = n == 4 ? counts4 : counts5;
        const int ncounts = n == 4 ? 12 : 6;
        shaper_build(&t, blocks, taps[profile], n, 3);
        for (int c = 0; c < ncounts; ++c) {
            for (unsigned seed = 0; seed < sizeof seeds / sizeof seeds[0]; ++seed) {
                const int count = counts[c], words = count * n;
                uint32_t x = seeds[seed];
                for (int i = 0; i < SHAPER_HISTORY + count; ++i) {
                    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                    symbols[i] = 16 * ((x >> 16) & 7);
                }
                const uint8_t *sym = symbols + SHAPER_HISTORY;
                shaper_fill_c(reference, sym, count, &t);
                for (int kernel = 0; kernel < (n == 4 ? 4 : 1); ++kernel) {
                    memset(guarded, 0xa5, sizeof guarded);
                    uint32_t *dst = guarded + 4;
                    const int groups = (count - 1) / 6, bulk = 6 * groups + 1;
                    if (count == 0 && kernel < 2) {
                        // Empty output for the raw kernels is a caller no-op.
                    } else if (n == 5) {
                        shaper_fill5_pie(dst, sym, count / 4, blocks);
                    } else {
                        if (kernel == 2) shaper_fill4_all_pie(dst, sym, count, blocks);
                        else if (kernel == 0) shaper_fill4_pie(dst, sym, groups, blocks);
                        else {
                            shaper_rows4(rows + 6, sym, count, blocks);
                            if (kernel == 3) shaper_fill4_rows_all_pie(dst, rows + 6, count);
                            else shaper_fill4_rows_pie(dst, rows + 6, groups);
                        }
                        if (kernel < 2) shaper_fill_c(dst + 4 * bulk, sym + bulk, count - bulk, &t);
                    }
                    unsigned mismatch = 0, guards = 1;
                    for (int i = 0; i < words; ++i) {
                        mismatch += dst[i] != reference[i];
                    }
                    for (int i = 0; i < 4; ++i) {
                        guards &= guarded[i] == 0xa5a5a5a5u && dst[words + i] == 0xa5a5a5a5u;
                    }
                    printf("@{\"test\":\"%s\",\"profile\":\"%s\",\"symbols\":%d,"
                           "\"seed\":%" PRIu32 ",\"mismatch\":%u,\"guards\":%u,"
                           "\"table_check\":%" PRIu32 ",\"check\":%" PRIu32 "}\n",
                           n == 5 ? "shape5_pie" : kernel == 0 ? "shape4_pie" : kernel == 1 ? "shape4_rows" : kernel == 2 ? "shape4_all" : "shape4_rows_all",
                           names[profile], count, seeds[seed], mismatch, guards,
                           checksum(blocks, shaper_table_words(n, 3)), checksum(dst, words));
                }
            }
        }
    }
    if (tx_samples_check()) { puts("TX-SAMPLES-FAIL"); }
    if (maxrate_check()) { puts("MAXRATE-FAIL"); }
    int round2_check(void);
    if (round2_check()) { puts("ROUND2-FAIL"); }
    int qpsk_fill_check(void);
    if (qpsk_fill_check()) puts("QPSK-FILL-FAIL");
    puts("QEMU-END");
}
