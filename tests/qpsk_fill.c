// Same guards, arbitrary slices and bank-sized calls on native C and ESP32-S3 QEMU.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "shaper_qpsk.h"

#ifdef QPSK_HOST
static uint32_t table[QPSK_TABLE_WORDS(4)] __attribute__((aligned(16)));
static uint32_t want[16384], guarded[16384+8] __attribute__((aligned(16)));
static uint8_t src[2+1368];
#else
extern uint32_t shaper_store[], test_reference[], test_guarded[];
extern uint8_t test_symbols[];
#define table shaper_store
#define want test_reference
#define guarded test_guarded
#define src test_symbols
#endif

static uint32_t hash(const uint32_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

int qpsk_fill_check(void)
{
    unsigned cases = 0, errors = 0;
    const unsigned levels[] = {16,128,508};
    const unsigned counts[] = {0,1,2,3,4,7,11,12,13,15,16,17,31,32,47,48,49,768,15740,16380};
    for (unsigned n = 3; n <= 4; ++n) {
        for (unsigned l = 0; l < 3; ++l) {
            if (shaper_qpsk_build(table,n,levels[l])) return 1;
            for (unsigned seed = 0; seed < 3; ++seed) {
                uint32_t x = seed == 0 ? 2026 : seed == 1 ? 7 : 0xffffffffu;
                for (unsigned i = 0; i < 2+1368; ++i) {
                    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                    src[i] = x >> 16;
                }
                shaper_qpsk_c(want,src+2,16384/(4*n),table,n);
                for (unsigned skip = 0; skip < 4*n; ++skip) {
                    for (unsigned c = 0; c < sizeof counts/sizeof *counts; ++c) {
                        unsigned count = counts[c];
                        if (skip+count > 16384/(4*n)*(4*n)) continue;
                        for (unsigned align = 0; align < 4; ++align) {
                            uint32_t *dst = guarded+4+align;
                            for (unsigned i = 0; i < 4; ++i) {
                                dst[(int)i-4] = 0xa5a5a5a5;
                                dst[count+i] = 0xa5a5a5a5;
                            }
                            shaper_qpsk_samples(dst,src+2,skip,count,table,n);
                            for (unsigned i = 0; i < count; ++i) errors += dst[i] != want[skip+i];
                            for (unsigned i = 0; i < 4; ++i) {
                                errors += dst[(int)i-4] != 0xa5a5a5a5;
                                errors += dst[count+i] != 0xa5a5a5a5;
                            }
                            ++cases;
                        }
                    }
                }
                printf("@{\"test\":\"qpsk_fill\",\"sps\":%u,\"level\":%u,\"seed\":%u,"
                       "\"table_check\":%" PRIu32 ",\"check\":%" PRIu32 ",\"errors\":%u}\n",
                       n,levels[l],seed,hash(table,QPSK_TABLE_WORDS(n)),
                       hash(want,16384/(4*n)*(4*n)),errors);
            }
        }
    }
    printf("@{\"test\":\"qpsk_fill_end\",\"cases\":%u,\"errors\":%u}\n",cases,errors);
    return errors != 0;
}
#ifdef QPSK_HOST
int main(void) { return qpsk_fill_check(); }
#endif
