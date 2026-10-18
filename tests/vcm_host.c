// Real transmitter lifecycle with the stream harness's peripheral stubs.
#define main fixed_stream_main
#define NATIVE_PACE_TEST
#include "stream_host.c"
#undef main

esp_err_t radio_s3_paced_run_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                                  void *ctx, radio_s3_stats_t *stats)
{ (void)sec; (void)rate; (void)fn; (void)ctx; (void)stats; abort(); }

esp_err_t radio_s3_paced_wifi_rate(uint32_t sec, uint32_t rate, radio_s3_fill_fn fn,
                                   void *ctx, radio_s3_stats_t *stats)
{
    assert(enc.vcm && sec == 1 && rate == 40000000);
    assert(radio_s3_stream_ready() == ESP_OK);
    radio_s3_dry_stats_t dry;
    memset(stats,0,sizeof *stats);
    // Exercise the real bank/oracle path; host clocks cannot model a one-second deadline.
    esp_err_t err=radio_s3_dry_run_rate(fn,ctx,100000,rate,discard,NULL,&dry);
    stats->fills=dry.fills;
    return err;
}

static void vcm_header(dvbs2_workspace *ws)
{
    ws->bits[0] &= ~16u;
    uint8_t crc=0;
    for (unsigned i=0;i<9;++i)
        for (unsigned b=0;b<8;++b) {
            const unsigned feedback=(crc>>7)^((ws->bits[i]>>(7-b))&1);
            crc=(crc<<1)^((feedback&1)?0xd5:0);
        }
    ws->bits[9]=crc;
}

int main(int argc, char **argv)
{
    assert(argc == 6 || argc == 7);
    const unsigned sps=atoi(argv[1]), hold=atoi(argv[2]);
    const bool control=hold == 0;
    const bool axes=argc == 7;
    const unsigned alternate[14]={4,12,5,13,6,14,7,15,8,16,9,17,10,11};
    const unsigned requested[4]={4,11,13,17};
    const unsigned *pattern=hold == 3 ? requested : alternate;
    const unsigned modes=axes && hold == 3 ? 4 : sps == 40 ? 8 : 14;
    const unsigned frames=control ? 32 : 2*modes*hold;
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    bank_shift=37; // physical wrap and symbol cuts disagree at both 4/8 sps
    callsign="";
    if (argc == 7) dvbtx_vcm_command("axes on");
    if (control) {
        native_input=PV_GCS;
        native_wire=fopen(argv[5],"rb"); assert(native_wire);
        dvbtx_vcm_command("on");
    } else {
        char command[160]; int at=snprintf(command,sizeof command,"%u",hold);
        for (unsigned i=0;i<modes;++i) at+=snprintf(command+at,sizeof command-at," %u",axes ? pattern[i] : i+4);
        dvbtx_vcm_command(command);
    }
    // Reference plain C and exported phases are checked independently in Python.
    phase_count=0;
    phases=malloc((2*frames+8)*DVBS2_MAX_SYMBOLS);
    FILE *expected=fopen(argv[4],"wb"); assert(expected);
    dvbs2_workspace ws;
    uint8_t payload[8100]; uint32_t seed=SEED;
    for (unsigned f=0;f<(control ? 2*frames+8 : frames+8);++f) {
        const unsigned mc=control ? (f ? 13 : 4) : axes ? pattern[(f/hold)%modes] : 4+(f/hold)%modes;
        const dvbs2_mode *mode=dvbs2_find_mode(mc);
        const size_t n=(mode->kbch-80)/8;
        for (size_t i=0;i<n;++i) payload[i]=next_byte(&seed);
        assert(fwrite(payload,1,n,expected) == n);
        assert(!dvbs2_bb_gcs(&ws,mode,payload,n*8)); vcm_header(&ws);
        dvbs2_bb_scramble(&ws,mode); dvbs2_bch(&ws,mode); dvbs2_ldpc(&ws,mode);
        dvbs2_map(&ws,mode,phases+phase_count+90); dvbs2_plframe(mode,1,phases+phase_count);
        phase_count+=dvbs2_frame_symbols(mode,1);
    }
    fclose(expected);
    FILE *symbols=fopen(argv[3],"wb"); assert(symbols);
    assert(fwrite(phases,1,phase_count,symbols) == phase_count); fclose(symbols);
    allocation_count=0;
    assert(start_encoder("dvbdump",4,128,0,sps,40000000));
    const int allocations=allocation_count;
    size_t internal=0;
    for (unsigned i=0;i<64;++i)
        if (native_blocks[i].p && (native_blocks[i].caps&MALLOC_CAP_INTERNAL)) internal+=native_blocks[i].n;
    printf("VCM n%u: internal heap %zu, table %zu, context %zu\n",sps,internal,
           dvb.table.parts == SHAPER_AXES_PARTS ? (size_t)SHAPER_AXES_BYTES :
           4*shaper_table_words(sps,sps == 40 ? 2 : 3),dvbs2_fast_size());
    if (argc == 7) {
        assert(sps == 8 && internal == 0 && !enc.block && dvb.samples_borrowed);
        const uint8_t *begin=(uint8_t *)shaper_store, *end=begin+sizeof shaper_store;
        assert(enc.slot[0] == begin+SHAPER_AXES_BYTES);
        assert(enc.slot[1] == enc.slot[0]+packed_bytes(DVBS2_MAX_SYMBOLS));
        assert(enc.filler == enc.slot[1]+packed_bytes(DVBS2_MAX_SYMBOLS));
        assert((uint8_t *)enc.ws == enc.filler+packed_bytes(DVBS2_DUMMY_SYMBOLS));
        assert(dvb.win == (uint8_t *)enc.ws+DVBS2_FAST_WORKSPACE_USED);
        assert((uint8_t *)dvb.samples >= dvb.win+dvb.win_bytes);
        assert((uint8_t *)(dvb.samples+dvb.cached_words) <= end);
    }
    stop_encoder(); printf("}\n"); assert(!live_allocations);
    if (control) { rewind(native_wire); }
    // All startup allocations fail cleanly, without touching the radio.
    if (!control) for (int i=0;i<allocations;++i) {
        fail_after=i;
        assert(!start_encoder("dvbdump",4,128,0,sps,40000000));
        assert(!live_allocations);
    }
    fail_after=-1;
    for (unsigned repeat=0;repeat<2;++repeat) {
        if (control) rewind(native_wire);
        char iq[4096]; snprintf(iq,sizeof iq,"%s.iq",argv[3]);
        capture=fopen(iq,"wb"); assert(capture);
        dvbtx_dump(4,frames,128,sps == 40 ? 10 : 1,0,sps,40000000);
        fclose(capture); assert(!live_allocations);
    }
    if (axes && hold == 3) {
        callsign="N0CALL";
        unsigned probes=wifi_probes;
        dvbtx_wifi(4,8,40000000);
        assert(wifi_probes == probes+1 && !live_allocations);
        callsign="";
        dvbtx_pace(4,1,128,0,8,40000000);
        assert(!live_allocations);
    }
    if (native_wire) fclose(native_wire);
    // Admission is explicit, including TS and high-rate rejection.
    native_input=PV_TS;
    assert(!start_encoder("dvbdump",4,128,0,4,40000000));
    native_input=0;
    assert(!start_encoder("dvbdump",4,128,0,4,80000000));
#ifdef NATIVE_INTERNAL_BUDGET
    dvbtx_vcm_command("axes off");
    assert(!start_encoder("dvbdump",4,128,0,8,40000000));
    assert(!live_allocations);
    puts("axes: 30 KB internal budget, no internal block over 2 KB; legacy allocation fails cleanly");
#endif
    dvbtx_vcm_command("off");
    free(phases); free(banks);
    return 0;
}
