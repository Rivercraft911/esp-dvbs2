// Replay the observed ID/data bank, including the real station and overlap muxes.
#include "native.h"
#define lofty_samples checked_samples
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#undef lofty_samples
void lofty_samples(uint32_t *, const uint32_t *, unsigned, size_t, const uint32_t *);
static unsigned bulk, singles;

void checked_samples(uint32_t *dst, const uint32_t *src, unsigned skip, size_t n, const uint32_t *table)
{
    uintptr_t address = (uintptr_t)dst;
    size_t left = n;
    unsigned cut = skip;
    while (left && (cut || (address&15) || left < 12)) {
        size_t take = left < 12-cut ? left : 12-cut;
        ++singles; address += 4*take; left -= take; cut = 0;
    }
    bulk += left/12;
    singles += left%12 != 0;
    lofty_samples(dst,src,skip,n,table);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    unsigned mc = atoi(argv[1]);
    callsign = "N0CALL";
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    assert(start_encoder("dvbpace",mc,128,0,3,80000000));
    station_id_t id;
    assert(station_id_init_rate(&id,callsign,80000000,80000000,fill,NULL));
    assert(id.length == 417600000);
    FILE *out = fopen(argv[2],"wb"); assert(out);
    const uint64_t start = 417597812, stop = id.length+2*22194*3;
    for (unsigned bank=0; ; ++bank) {
        uint64_t first = start+(uint64_t)bank*15740;
        size_t offset = first%RADIO_S3_BANK_WORDS, head = RADIO_S3_BANK_WORDS-offset;
        uint32_t *dst = banks[bank&1];
        assert(!station_id_fill(dst+offset,first,head,&id));
        assert(!station_id_fill(dst,first+head,offset,&id));
        if (!bank) {
            assert(lofty_mux.handoff[0].words == 8100 && lofty_mux.handoff[1].words == 6092);
            assert(bulk == 1182 && singles == 1); // no per-block alignment fallback
            assert(!lofty_mux.startup && !lofty_mux.steady);
        }
        // Keep only the nonoverlapping data interval from each physical bank.
        uint64_t begin = first < id.length ? id.length : first;
        uint64_t end = first+15740 < stop ? first+15740 : stop;
        for (uint64_t n=begin; n<end; ++n) {
            uint32_t w = dst[n%RADIO_S3_BANK_WORDS];
            int16_t iq[] = {(int16_t)((int32_t)(w<<22)>>22),(int16_t)((int32_t)(w<<12)>>22)};
            assert(fwrite(iq,sizeof iq,1,out) == 1);
        }
        if (end == stop) break;
    }
    fclose(out);
    assert(!dvb.queue_empty && !dvb.ring.underruns);
    stop_encoder(); printf("}\n");
    assert(!live_allocations);
    free(banks);
    return 0;
}
