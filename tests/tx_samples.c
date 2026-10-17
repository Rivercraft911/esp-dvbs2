// Exercise the transmitter's raw-phase shaper glue on host and QEMU. No peripherals.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "shaper.h"
#include "shaper_taps.h"

uint32_t test_reference[16440] __attribute__((aligned(16)));
uint32_t test_guarded[4 + 16384 + 4] __attribute__((aligned(16)));
uint8_t test_symbols[SHAPER_HISTORY + 4100];
#define ref test_reference
#define guarded test_guarded
#define raw test_symbols
static uint8_t shifted[sizeof raw];

static uint32_t check(const uint32_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

int tx_samples_check(void)
{
    const unsigned ns[] = {4, 8, 40};
    const int32_t *taps[] = {shaper_taps_8psk_n4_a35, shaper_taps_8psk_n8_a35,
                              shaper_taps_qpsk_n40_a35};
    int bad = 0;
    for (unsigned profile = 0; profile < 3; ++profile) {
        const unsigned n = ns[profile];
        shaper_table_t t;
        shaper_build(&t, shaper_store, taps[profile], n, n == 40 ? 2 : 3);
        uint32_t x = 2026;
        for (unsigned i = 0; i < sizeof raw; ++i) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            raw[i] = n == 40 ? 2 * ((x >> 16) & 3) + 1 : (x >> 16) & 7;
            shifted[i] = raw[i] << 4;
        }
        const uint8_t *sym = raw + SHAPER_HISTORY;
        // Every possible bank-edge symbol offset and every tail residue, including count 0.
        // dst is placed as word first%16380, so alignment matches actual bank memory.
        unsigned mismatch = 0, guards = 1, cases = 0;
        uint32_t hash = 2166136261u;
        for (unsigned skip = 0; skip < n; ++skip) {
            const unsigned offset = skip % 4;
            for (unsigned c = 0; c < 20; ++c) {
                const unsigned count = c < 19 ? c : 16380 - offset;
                const unsigned symbols = (skip + count + n - 1) / n;
                shaper_fill_c(ref, shifted + SHAPER_HISTORY, symbols, &t);
                memset(guarded, 0xa5, sizeof guarded);
                uint32_t *dst = guarded + 4 + offset;
                stream_profile_t whole = {0}, partial = {0};
                shaper_samples_profile(dst, sym, skip, count, &t, &whole, &partial);
                const unsigned head = skip && count ? (n - skip < count ? n - skip : count) : 0;
                mismatch += whole.calls != ((count - head) / n != 0);
                mismatch += partial.calls != (unsigned)(head != 0) + ((count - head) % n != 0);
                for (unsigned i = 0; i < count; ++i) mismatch += dst[i] != ref[skip + i];
                for (unsigned i = 0; i < 4 + offset; ++i) guards &= guarded[i] == 0xa5a5a5a5u;
                for (unsigned i = 0; i < 4; ++i) guards &= dst[count + i] == 0xa5a5a5a5u;
                hash = (hash ^ check(dst, count)) * 16777619u;
                ++cases;
            }
        }
        // The real bank schedule, including its 16380%8 == 4 half-symbol wrap.
        for (unsigned b = 0; b < 80; ++b) {
            const uint64_t first = b ? (uint64_t)b * 14268 - 64 : 0;
            const unsigned offset = first % 16380;
            const unsigned counts[] = {16380 - offset, offset};
            uint64_t at = first;
            for (unsigned c = 0; c < 2; ++c) {
                const unsigned skip = at % n, count = counts[c];
                shaper_fill_c(ref, shifted + SHAPER_HISTORY, (skip + count + n - 1) / n, &t);
                memset(guarded, 0xa5, sizeof guarded);
                uint32_t *dst = guarded + 4 + at % 4;
                shaper_samples(dst, sym, skip, count, &t);
                for (unsigned i = 0; i < count; ++i) mismatch += dst[i] != ref[skip + i];
                guards &= dst[count] == 0xa5a5a5a5u;
                hash = (hash ^ check(dst, count)) * 16777619u;
                at += count;
                ++cases;
            }
        }
        printf("@{\"test\":\"tx_samples\",\"sps\":%u,\"cases\":%u,\"mismatch\":%u,"
               "\"guards\":%u,\"check\":%" PRIu32 "}\n", n, cases, mismatch, guards, hash);
        bad |= mismatch || !guards;
    }
    return bad;
}
#ifdef TX_HOST
int main(void) { return tx_samples_check(); }
#endif
