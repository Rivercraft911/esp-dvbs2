// ESP-IDF/QEMU harness for shaper_fill4_pie. No radio or board access.
// Build with shaper.c and shaper_pie.S, then pass the UART log to tests/test_shaper.py.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "shaper.h"
#include "shaper_taps.h"

static uint32_t blocks[8192] __attribute__((aligned(16)));
static uint32_t reference[16380];
static uint32_t guarded[4 + 16380 + 4] __attribute__((aligned(16)));
static uint8_t symbols[SHAPER_HISTORY + 4095];

static uint32_t checksum(const uint32_t *p, size_t words)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void app_main(void)
{
    static const int counts[] = {1, 7, 13, 4093, 4095};
    static const uint32_t seeds[] = {2026, 7, UINT32_MAX};
    static const int32_t *taps[] = {shaper_taps_8psk_n4_a20, shaper_taps_8psk_n4_a35};
    static const char *names[] = {"8psk-n4-a20", "8psk-n4-a35"};
    puts("QEMU-START");
    for (int profile = 0; profile < 2; ++profile) {
        shaper_table_t t;
        shaper_build(&t, blocks, taps[profile], 4, 3);
        for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; ++c) {
            for (unsigned seed = 0; seed < sizeof seeds / sizeof seeds[0]; ++seed) {
                const int count = counts[c], words = count * 4;
                uint32_t x = seeds[seed];
                for (int i = 0; i < SHAPER_HISTORY + count; ++i) {
                    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                    symbols[i] = 16 * ((x >> 16) & 7);
                }
                const uint8_t *sym = symbols + SHAPER_HISTORY;
                shaper_fill_c(reference, sym, count, &t);
                memset(guarded, 0xa5, sizeof guarded);
                uint32_t *dst = guarded + 4;
                const int groups = (count - 1) / 6, bulk = 6 * groups + 1;
                shaper_fill4_pie(dst, sym, groups, blocks);
                shaper_fill_c(dst + 4 * bulk, sym + bulk, count - bulk, &t);
                unsigned mismatch = 0, guards = 1;
                for (int i = 0; i < words; ++i) {
                    mismatch += dst[i] != reference[i];
                }
                for (int i = 0; i < 4; ++i) {
                    guards &= guarded[i] == 0xa5a5a5a5u && dst[words + i] == 0xa5a5a5a5u;
                }
                printf("@{\"test\":\"shape4_pie\",\"profile\":\"%s\",\"symbols\":%d,"
                       "\"seed\":%" PRIu32 ",\"mismatch\":%u,\"guards\":%u,"
                       "\"table_check\":%" PRIu32 ",\"check\":%" PRIu32 "}\n",
                       names[profile], count, seeds[seed], mismatch, guards,
                       checksum(blocks, shaper_table_words(4, 3)), checksum(dst, words));
            }
        }
    }
    puts("QEMU-END");
}
