// The same checks run as a host executable and on the board's PIE code (tests/tx_pie_bench).
#include "shaper.h"
#include "shaper_taps.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t axes[SHAPER_AXES_BYTES/4] __attribute__((aligned(16)));
static uint32_t full[SHAPER_STORE_WORDS] __attribute__((aligned(16)));
static uint32_t qpsk[2048] __attribute__((aligned(16)));
static uint32_t want[264*8] __attribute__((aligned(16)));
static uint32_t got[264*8+16] __attribute__((aligned(16)));
static uint8_t symbols[8+264+16] __attribute__((aligned(16)));
static uint32_t seed=2026;

static unsigned random_phase(void)
{
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    return seed&7;
}

static void compare(const shaper_table_t *a, const shaper_table_t *f, unsigned n)
{
    shaper_fill_c(want,symbols+8,n,f);
    for (unsigned skip=0;skip<8;++skip) {
        for (unsigned tail=0;tail<8;++tail) {
            unsigned count=n*8 > skip+tail ? n*8-skip-tail : 0;
            for (unsigned i=0;i<sizeof got/sizeof *got;++i) got[i]=0xdeadbeef;
            shaper_axes_samples_profile(got+skip,symbols+8,skip,count,a,NULL,NULL);
            assert(!memcmp(got+skip,want+skip,count*4));
            for (unsigned i=0;i<skip;++i) assert(got[i] == 0xdeadbeef);
            for (unsigned i=skip+count;i<sizeof got/sizeof *got;++i) assert(got[i] == 0xdeadbeef);
        }
    }
}

static void check(void)
{
    for (unsigned level=16;level<=508;level=level == 16 ? 128 : level == 128 ? 508 : 509) {
        int32_t taps[72];
        for (unsigned i=0;i<72;++i) taps[i]=(int32_t)((double)shaper_taps_8psk_n8_a35[i]*level/718.4);
        shaper_table_t a,f,q;
        shaper_axes_build(&a,axes,taps);
        shaper_build(&f,full,taps,8,3);
        shaper_build(&q,qpsk,taps,8,2);
        // Every complex three-symbol window in each of the three pulse parts.
        for (unsigned part=0;part<3;++part)
            for (unsigned w=0;w<512;++w) {
                memset(symbols,0,sizeof symbols);
                for (unsigned j=0;j<3;++j) symbols[8-3*part-j]=((w>>(3*j))&7)*16;
                compare(&a,&f,1);
            }
        // All six-way rotations, tails, sample offsets and vector guard words.
        for (unsigned n=0;n<=257;++n) {
            for (unsigned i=0;i<sizeof symbols;++i) symbols[i]=16*random_phase();
            compare(&a,&f,n);
        }
        // The odd-phase subset agrees exactly with QPSK's own rounded table.
        for (unsigned i=0;i<sizeof symbols;++i) symbols[i]=16*(random_phase()|1);
        compare(&a,&q,257);
    }
    puts("axes: exhaustive windows, levels 16/128/508, QPSK subset, zero/partial/tails and guards: ok");
}

#ifdef __XTENSA__
void app_main(void) { check(); puts("QEMU-END"); }
#else
int main(void) { check(); return 0; }
#endif
