// Portable/Python and PIE comparisons use identical inputs, including lookahead.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "shaper.h"
#ifdef SHORT_HOST
static uint32_t reference[16440], guarded[16392];
#else
extern uint32_t test_reference[16440], test_guarded[16392];
#define reference test_reference
#define guarded test_guarded
#endif
static uint8_t symbols[4104];
static uint32_t hash(const uint32_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
int short_check(void)
{
    const unsigned counts[] = {0,1,2,3,4,5,6,7,8,23,24,25,26,31,47,48,49,4095};
    const unsigned levels[] = {16,128,508};
    const uint32_t seeds[] = {2026,7,UINT32_MAX};
    int bad = 0;
    for (unsigned level = 0; level < 3; ++level) {
        shaper_table_t t;
        shaper_short_build(&t, shaper_store, levels[level]);
        for (unsigned seed = 0; seed < 3; ++seed) {
            uint32_t x = seeds[seed];
            for (unsigned i = 0; i < sizeof symbols; ++i) {
                x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                symbols[i] = ((x >> 16) & 7) << 4;
            }
            for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; ++c) {
                const unsigned n = counts[c], words = n*4;
                memset(guarded, 0xa5, sizeof(uint32_t)*16392);
                shaper_fill_c(reference, symbols+8, n, &t);
                unsigned raw_bad = 0;
                if (n <= 8) {
                    uint8_t raw[17];
                    for (unsigned i = 0; i < sizeof raw; ++i) raw[i] = symbols[i] >> 4;
                    shaper_fill_raw(guarded+4, raw+8, n, &t);
                    raw_bad = memcmp(guarded+4, reference, words*4) != 0;
                    memset(guarded, 0xa5, sizeof(uint32_t)*16392);
                }
#ifdef SHORT_HOST
                shaper_fill_c(guarded+4, symbols+8, n, &t);
#else
                shaper_short4_pie(guarded+4, symbols+8, n, t.blocks);
#endif
                unsigned mismatch = raw_bad, guards = 1;
                for (unsigned i = 0; i < words; ++i) mismatch += guarded[4+i] != reference[i];
                for (unsigned i = 0; i < 4; ++i)
                    guards &= guarded[i] == 0xa5a5a5a5 && guarded[4+words+i] == 0xa5a5a5a5;
                bad |= mismatch || !guards;
                printf("@{\"test\":\"short\",\"symbols\":%u,\"peak\":%u,\"seed\":%"PRIu32
                       ",\"mismatch\":%u,\"guards\":%u,\"table\":%"PRIu32",\"check\":%"PRIu32"}\n",
                       n,levels[level],seeds[seed],mismatch,guards,hash(shaper_store,4096),
                       hash(guarded+4,words));
            }
        }
    }
    return bad;
}
#ifdef SHORT_HOST
int main(void) { return short_check(); }
#endif
