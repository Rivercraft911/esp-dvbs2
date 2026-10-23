#include "lofty.h"
#include "machine.h"
#include "dvbs2.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t table[LOFTY_TABLE_WORDS] __attribute__((aligned(16)));
static uint32_t got[16420] __attribute__((aligned(16)));
static uint32_t want[16420] __attribute__((aligned(16)));
static uint32_t offsets[1380] __attribute__((aligned(16)));
static uint16_t words[1380] __attribute__((aligned(16)));

static void page_offsets(void)
{
    for (unsigned page = 10; page-- > 0;) {
        memmove(offsets+132*page,offsets+128*page,128*sizeof *offsets);
        for (unsigned k = 128; k < 132; ++k) offsets[132*page+k] = 0xa5a50000+page;
    }
}

static uint32_t hash(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    while (n--) h = (h^*p++)*16777619u;
    return h;
}

static void reference(uint32_t *dst, unsigned blocks)
{
    // Independent of the PIE pointer pipeline and wrapper, including history.
    for (unsigned b = 0; b < blocks; ++b) {
        uint32_t h = (uint32_t)words[b]<<12 | words[b+1];
        for (unsigned p = 0; p < 12; ++p)
            *dst++ = ((p < 4 ? table[4*((h>>15)&511)+p] : table[2048+8*((h>>15)&63)+p-4]) +
                      table[2560+12*((h>>6)&511)+p] +
                      (p < 4 ? 0 : table[(p < 8 ? 8704 : 8960)+4*(h&63)+(p&3)]))^0x80200;
    }
}

static void kernels(void)
{
    unsigned cases = 0;
    for (unsigned peak = 0; peak < 3; ++peak)
        for (unsigned seed = 0; seed < 3; ++seed) {
            assert(!lofty_build(table,(unsigned[]){16,128,508}[peak]));
            uint32_t x = (uint32_t[]){2026,7,UINT32_MAX}[seed];
            for (unsigned i = 0; i < 1380; ++i) {
                x ^= x<<13; x ^= x>>17; x ^= x<<5;
                words[i] = x&4095;
            }
            reference(want,1367);
            for (unsigned source = 0; source < 8; ++source)
              for (unsigned align = 0; align < 4; ++align) {
                memset(offsets,0xa5,sizeof offsets);
                uint32_t old = source ? words[source-1] : 0;
                uint32_t previous = lofty_prepare(offsets+align,words+source,1367,old);
                assert(previous == words[source+1366]);
                for (unsigned j = 0; j < 1367; ++j) {
                    uint32_t h = old<<12 | words[source+j];
                    uint32_t expected = (((h>>15)&511)<<4) | (((h>>6)&511)*3<<13) | ((h&63)<<24);
                    assert(offsets[align+j] == expected);
                    old = words[source+j];
                }
                for (unsigned j = 0; j < align; ++j) assert(offsets[j] == 0xa5a5a5a5);
                for (unsigned j = align+1367; j < 1380; ++j) assert(offsets[j] == 0xa5a5a5a5);
            }
            assert(lofty_prepare(offsets,words+1,1367,words[0]) == words[1367]);
            offsets[1367] = 0;
            const unsigned lengths[] = {0,1,11,12,13,47,48,49,15740,16380};
            for (unsigned skip = 0; skip < 12; ++skip)
                for (unsigned align = 0; align < 4; ++align)
                    for (unsigned k = 0; k < sizeof lengths/sizeof *lengths; ++k) {
                        unsigned n = lengths[k], first = 4+align;
                        memset(got,0xa5,sizeof got);
                        lofty_samples(got+first,offsets,skip,n,table);
                        assert(!memcmp(got+first,want+skip,4*n));
                        for (unsigned i = 0; i < first; ++i) assert(got[i] == 0xa5a5a5a5);
                        for (unsigned i = first+n; i < 16420; ++i) assert(got[i] == 0xa5a5a5a5);
                        ++cases;
                    }
            page_offsets();
            uint32_t source_hash = hash(offsets,1320*sizeof *offsets);
            for (unsigned pages = 0; pages <= 10; ++pages) {
                memset(got,0xa5,sizeof got);
                uint32_t consumed = UINT32_MAX-4;
                lofty_render_pages(got+4,offsets,pages,table,&consumed);
                assert(consumed == UINT32_MAX-4+pages);
                assert(!memcmp(got+4,want,pages*1536*sizeof *got));
                assert(source_hash == hash(offsets,1320*sizeof *offsets));
                for (unsigned k = 0; k < 4; ++k) assert(got[k] == 0xa5a5a5a5);
                for (unsigned k = 4+1536*pages; k < 16420; ++k) assert(got[k] == 0xa5a5a5a5);
            }
            printf("@{\"test\":\"lofty\",\"peak\":%u,\"seed\":%u,\"table\":%"PRIu32",\"samples\":%"PRIu32"}\n",
                   (unsigned[]){16,128,508}[peak],seed,hash(table,sizeof table),hash(want,1367*48));
        }
    printf("@{\"test\":\"lofty_end\",\"slices\":%u,\"errors\":0}\n",cases);
    puts("lofty pages: 99 guarded batches, metadata skipped, release counter wraps");
    // I/Q fields do not overlap, so the fused ADD renderer must equal OR.
    const unsigned shifts[] = {3,2,0};
    for (unsigned level = 0; level < 3; ++level) {
        assert(!machine_build(table,(unsigned[]){16,128,508}[level]));
        uint32_t x = 2026;
        for (unsigned b = 0; b < 1365; ++b) {
            x ^= x<<13; x ^= x>>17; x ^= x<<5;
            offsets[b] = x&0x7ff07ff0;
            for (unsigned v = 0; v < 3; ++v)
                for (unsigned p = 0; p < 4; ++p)
                    want[12*b+4*v+p] = table[4*(256*v+((x>>(shifts[v]+4))&255))+p] |
                        table[4*(256*(3+v)+((x>>(shifts[v]+20))&255))+p];
        }
        offsets[1365] = 0;
        machine_render(got,offsets,1365,table);
        assert(!memcmp(got,want,1365*48));
    }
    puts("machine add: 4095 blocks equal the independent OR kernel");
}

static void encoder(void)
{
    dvbs2_fast *f = aligned_alloc(16,(dvbs2_fast_size()+31)&~(size_t)15);
    dvbs2_workspace *ws = aligned_alloc(16,(sizeof *ws+15)&~(size_t)15);
    uint8_t *payload = aligned_alloc(16,8112), *raw = aligned_alloc(16,33312);
    uint32_t *map = aligned_alloc(16,32768), *cache = aligned_alloc(16,32768);
    uint8_t *bits = malloc(DVBS2_FEC_BYTES);
    assert(f && ws && payload && raw && map && cache && bits);
    const unsigned sizes[] = {0,1,2,3,7,128,131};
    unsigned cases = 0;
    for (unsigned mc = 12; mc <= 17; ++mc)
        for (unsigned pilots = 0; pilots < 2; ++pilots) {
            const dvbs2_mode *m = dvbs2_find_mode(mc);
            unsigned n = dvbs2_frame_symbols(m,pilots), bytes = (m->kbch-80)/8;
            dvbs2_sign_state state = {0};
            uint32_t h = 0, x = 2026+mc;
            unsigned pending = 0, call = 0;
            for (unsigned frame = 0; frame < 3; ++frame) {
                for (unsigned j = 0; j < bytes; ++j) { x ^= x<<13; x ^= x>>17; x ^= x<<5; payload[j] = x; }
                uint8_t *guard = (uint8_t *)f+dvbs2_fast_mode_size(m);
                memset(guard,0xa5,16);
                assert(!dvbs2_fast_prepare(f,m,pilots,15));
                assert(!dvbs2_encode_frame(f,ws,NULL,payload,8*bytes,raw));
                memcpy(bits,ws->bits,DVBS2_FEC_BYTES);
                assert(dvbs2_fast_cache_size(f) <= 32768);
                assert(!dvbs2_fast_cache_tables(f,(uint8_t *)cache));
                if (m->t > 8 && !(m->kbch%16)) {
                    memset((uint8_t *)map+1024+DVBS2_BCH_PAIR_BYTES,0xa5,16);
                    assert(!dvbs2_fast_cache_bch_pair(f,(uint8_t *)map+1024));
                    for (unsigned j = 0; j < 16; ++j)
                        assert(((uint8_t *)map)[1024+DVBS2_BCH_PAIR_BYTES+j] == 0xa5);
                }
                assert(!dvbs2_fast_bb_gcs(f,ws,payload,8*bytes));
                dvbs2_fast_bch(f,ws);
                assert(!memcmp(bits,ws->bits,m->nbch/8));
                dvbs2_fast_ldpc(f,ws);
                assert(!memcmp(bits,ws->bits,DVBS2_FEC_BYTES));
                assert(!dvbs2_fast_lofty_cache(f,map));
                assert(!dvbs2_fast_lofty_start(f,ws,&state));
                unsigned at = 0;
                while (state.at < n) {
                    unsigned room = sizes[call++%7], got_n = room;
                    const uint16_t *out = dvbs2_fast_lofty_read(f,&state,&got_n);
                    assert(got_n <= room);
                    for (unsigned j = 0; j < got_n; ++j) {
                        do { h = ((h<<3)|raw[at++])&4095; } while (++pending < 4);
                        pending = 0;
                        if (out[j] != h)
                            fprintf(stderr,"mapper %u pilots %u frame %u at %u: %03x != %03x\n",
                                    mc,pilots,frame,at,out[j],(unsigned)h);
                        assert(out[j] == h);
                    }
                }
                while (at < n) { h = ((h<<3)|raw[at++])&4095; ++pending; }
                if (state.pending != pending || state.history != h)
                    fprintf(stderr,"end %u pilots %u frame %u: %u/%03x != %u/%03x\n",
                            mc,pilots,frame,state.pending,(unsigned)state.history,pending,(unsigned)h);
                assert(state.pending == pending && state.history == h);
                for (unsigned j = 0; j < 16; ++j) assert(guard[j] == 0xa5);
                ++cases;
            }
        }
    printf("@{\"test\":\"lofty_mapper\",\"frames\":%u,\"errors\":0}\n",cases);

    free(bits); free(cache); free(map); free(raw); free(payload); free(ws); free(f);
    void sign_mapper_check(void);
    sign_mapper_check();
}

int main(void) { kernels(); encoder(); return 0; }
