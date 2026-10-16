// Host build of firmware/main/shaper.c for tests/test_shaper.py: builds the device tables and
// runs the portable kernel on membench's symbols, printing the checks membench reports.
#include <stdio.h>
#include <stdlib.h>

#include "shaper.h"
#include "shaper_taps.h"

static uint32_t checksum(const uint32_t *p, size_t words)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

int main(void)
{
    static const struct {
        const char *name;
        const int32_t *taps;
        int n, bits, nsym;
    } cases[] = {
        {"shape8", shaper_taps_8psk_n8_a20, 8, 3, 2047},
        {"shape40", shaper_taps_qpsk_n40_a20, 40, 2, 409},
    };
    static uint32_t blocks[16384], bank[16384];
    static uint8_t sym[SHAPER_HISTORY + 2047];
    for (int c = 0; c < 2; ++c) {
        const int n = cases[c].n, bits = cases[c].bits, nsym = cases[c].nsym;
        shaper_table_t t;
        shaper_build(&t, blocks, cases[c].taps, n, bits);
        printf("%s_build %u\n", cases[c].name, checksum(blocks, shaper_table_words(n, bits)));
        uint32_t x = 2026;
        for (int i = 0; i < SHAPER_HISTORY + nsym; ++i) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            sym[i] = 16 * (bits == 3 ? (x >> 16) & 7 : 2 * ((x >> 16) & 3) + 1);
        }
        shaper_fill_c(bank, sym + SHAPER_HISTORY, nsym, &t);
        printf("%s_c %u\n", cases[c].name, checksum(bank, (size_t)nsym * n));
    }
    return 0;
}
