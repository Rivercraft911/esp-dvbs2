// Replay the observed ID/data bank, including the real station and overlap muxes.
#include "native.h"
#define lofty_samples checked_samples
#define main stream_lifecycle_main
#include "stream_host.c"
#undef main
#undef lofty_samples
void lofty_samples(uint32_t *, const uint32_t *, unsigned, size_t, const uint32_t *);
static unsigned bulk, singles, bank_switches;
static bool dummy_source;

void checked_samples(uint32_t *dst, const uint32_t *src, unsigned skip, size_t n, const uint32_t *table)
{
    if (dummy_source != lofty_mux.filler) {
        dummy_source = lofty_mux.filler;
        bank_switches |= dummy_source ? 1 : 2;
        printf("@{\"test\":\"source\",\"relative\":%llu,\"kind\":\"%s\"}\n",
               (unsigned long long)lofty_mux.output,dummy_source ? "dummy" : "data");
    }
    uintptr_t address = (uintptr_t)dst;
    // Every hardware slice reaches the bulk loop after at most one prefix.
    assert((skip&3) == ((address/4)&3));
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
    const bool forced = getenv("FORCE_DUMMY_FRAME") != NULL;
    const uint64_t start = 417597812, stop = id.length+(forced ? 6 : 2)*22194*3+(forced ? 2*9990 : 0);
    unsigned transitions = 0, all_switches = 0;
    unsigned old_frames = 0, old_dummies = 0;
    for (unsigned bank=0; ; ++bank) {
        uint64_t first = start+(uint64_t)bank*15740;
        size_t offset = first%RADIO_S3_BANK_WORDS, head = RADIO_S3_BANK_WORDS-offset;
        uint32_t *dst = banks[bank&1];
        bulk = singles = bank_switches = 0;
        assert(!station_id_fill(dst+offset,first,head,&id));
        assert(!station_id_fill(dst,first+head,offset,&id));
        if (!bank) {
            assert(lofty_mux.handoff[0].words == 8100 && lofty_mux.handoff[1].words == 6092);
            assert(bulk == 1182 && singles == 1); // no per-block alignment fallback
            assert(!lofty_mux.startup && !lofty_mux.steady);
        }
        unsigned dummies = lofty_mux.startup+lofty_mux.steady;
        if (lofty_mux.frames != old_frames || dummies != old_dummies || bank_switches) {
            printf("@{\"test\":\"transition\",\"first\":%llu,\"relative\":%llu,"
                   "\"data_before\":%u,\"data_after\":%u,\"dummy_before\":%u,\"dummy_after\":%u,"
                   "\"bulk\":%u,\"singles\":%u,\"switches\":%u}\n",
                   (unsigned long long)first,(unsigned long long)(first-id.length),
                   old_frames,(unsigned)lofty_mux.frames,old_dummies,dummies,bulk,singles,bank_switches);
            ++transitions;
        }
        all_switches |= bank_switches;
        old_frames = lofty_mux.frames; old_dummies = dummies;
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
    assert(!forced || (lofty_mux.startup == 2 && !lofty_mux.steady && transitions >= 6 && all_switches == 3));
    stop_encoder(); printf("}\n");
    assert(!live_allocations);
    free(banks);
    return 0;
}
