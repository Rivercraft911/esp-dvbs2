// Same block kernel on the host and in QEMU; no peripherals or timing claims.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaper.h"
#include "shaper_block.h"

#ifdef MAXRATE_HOST
static uint32_t reference[16380], guarded[16388];
#else
extern uint32_t test_reference[16440], test_guarded[16392];
#define reference test_reference
#define guarded test_guarded
#endif
static uint16_t packed[1367];

static uint32_t hash(const uint32_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

int maxrate_check(void)
{
    uint32_t *extra = aligned_alloc(16, SHAPER_BLOCK_WORDS * 4);
    if (!extra) { puts("MAXRATE-ALLOC-FAIL"); return 1; }
    uint32_t *table[] = {shaper_store, shaper_store + SHAPER_BLOCK_WORDS, extra};
    shaper_block_build(table);
    const unsigned counts[] = {0, 1, 2, 3, 4, 7, 31, 1365};
    const uint32_t seeds[] = {2026, 7, UINT32_MAX};
    int bad = 0;
    for (unsigned seed = 0; seed < 3; ++seed) {
        uint32_t x = seeds[seed];
        for (unsigned i = 0; i < 1367; ++i) {
            unsigned w = 0;
            for (unsigned k = 0; k < 4; ++k) {
                x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                w = (w << 3) | ((x >> 16) & 7);
            }
            packed[i] = w;
        }
        for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; ++c) {
            const unsigned count = counts[c], words = 12 * count;
            memset(guarded, 0xa5, sizeof(uint32_t) * 16388);
            shaper_block_c(reference, packed + 2, count, table[0], table[1], table[2]);
#ifdef MAXRATE_HOST
            shaper_block_c(guarded + 4, packed + 2, count, table[0], table[1], table[2]);
#else
            shaper_block_pie(guarded + 4, packed + 2, count, table[0], table[1], table[2]);
#endif
            unsigned mismatch = 0, guards = 1;
            for (unsigned i = 0; i < words; ++i) mismatch += guarded[4+i] != reference[i];
            for (unsigned i = 0; i < 4; ++i)
                guards &= guarded[i] == 0xa5a5a5a5 && guarded[4+words+i] == 0xa5a5a5a5;
            bad |= mismatch || !guards;
            printf("@{\"test\":\"maxrate\",\"groups\":%u,\"seed\":%" PRIu32
                   ",\"mismatch\":%u,\"guards\":%u,\"table\":[%" PRIu32 ",%" PRIu32
                   ",%" PRIu32 "],\"check\":%" PRIu32 "}\n", count, seeds[seed], mismatch,
                   guards, hash(table[0], SHAPER_BLOCK_WORDS), hash(table[1], SHAPER_BLOCK_WORDS),
                   hash(table[2], SHAPER_BLOCK_WORDS), hash(guarded+4, words));
        }
    }
    free(extra);
    return bad;
}

#ifdef MAXRATE_HOST
int main(void) { return maxrate_check(); }
#endif
