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

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        const int32_t *taps;
        int n, bits, nsym;
    } cases[] = {
        {"shape4", shaper_taps_8psk_n4_a20, 4, 3, 4093},
        {"shape4a35", shaper_taps_8psk_n4_a35, 4, 3, 4093},
        {"shape8", shaper_taps_8psk_n8_a20, 8, 3, 2047},
        {"shape40", shaper_taps_qpsk_n40_a20, 40, 2, 409},
    };
    static uint32_t blocks[16384], bank[16384];
    static uint8_t sym[SHAPER_HISTORY + 4093];
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
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
        if (argc == 2) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s.bin", argv[1], cases[c].name);
            FILE *f = fopen(path, "wb");
            if (!f) {
                return 1;
            }
            fwrite(blocks, sizeof *blocks, shaper_table_words(n, bits), f);
            fwrite(bank, sizeof *bank, (size_t)nsym * n, f);
            fclose(f);
        }
        // One-symbol calls must preserve the history across every split, with no tail writes.
        // n=4 exercises the C remainder needed after the PIE kernel's 6g+1 symbols.
        const uint32_t want = checksum(bank, (size_t)nsym * n);
        bank[nsym * n] = 0xdeadbeefu;
        shaper_fill_c(bank, sym + SHAPER_HISTORY, 0, &t);
        if (checksum(bank, (size_t)nsym * n) != want) {
            return 2;
        }
        for (int i = 0; i < nsym; ++i) {
            shaper_fill_c(bank + i * n, sym + SHAPER_HISTORY + i, 1, &t);
        }
        if (checksum(bank, (size_t)nsym * n) != want || bank[nsym * n] != 0xdeadbeefu) {
            return 3;
        }
    }
    return 0;
}
