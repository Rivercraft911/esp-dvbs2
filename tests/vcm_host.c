// Real transmitter lifecycle with the stream harness's peripheral stubs.
#define NATIVE_PACE_TEST
#define main fixed_stream_main
#include "stream_host.c"
#undef main

esp_err_t radio_s3_paced_run(uint32_t seconds, uint32_t rate, radio_s3_fill_fn fn, void *ctx,
                             const radio_s3_options_t *options, radio_s3_stats_t *stats)
{
    assert(seconds == 1 && rate == 40000000 && fn == fill_vcm);
    memset(stats,0,sizeof *stats);
    radio_s3_dry_stats_t dry;
    const esp_err_t err = radio_s3_dry_run_rate(fn,ctx,2ull*dvb.ring.frame_symbols*dvb.table.n,
                                                rate,discard,NULL,&dry);
    if (options->status) options->status(stats,23610); // one status line, as the command task's
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
    assert(argc == 7);
    const unsigned sps=atoi(argv[1]), hold=atoi(argv[2]);
    const bool control=hold == 0;
    const bool ota=!strcmp(argv[6],"ota"), mixed=ota || !strcmp(argv[6],"mixed"), first=!strcmp(argv[6],"first");
    const unsigned initial=!strcmp(argv[6],"8psk") ? 12 : 4;
    const unsigned modes=ota ? 4 : first ? 3 : mixed ? 14 : initial == 12 ? 6 : 8;
    const unsigned target=mixed ? 13 : initial == 12 ? 17 : 11;
    native_internal_limit=0; native_internal_largest=0;
    native_external_limit=1024*1024;
    if (sps == 8 && initial == 12) {
        native_internal_limit=16384; native_internal_largest=12288;
    }
    // GCS input takes its slot block from internal run memory too (the CRC table came with spiinput).
    const size_t ingress=control ? INGRESS_SLOTS*INGRESS_SLOT_BYTES : 0;
    native_internal_limit+=ingress;
    if (native_internal_largest < INGRESS_SLOTS*INGRESS_SLOT_BYTES) native_internal_largest=control ? INGRESS_SLOTS*INGRESS_SLOT_BYTES : native_internal_largest;
    const unsigned frames=control || ota ? 32 : 2*modes*hold;
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    bank_shift=37; // physical wrap and symbol cuts disagree at both 4/8 sps
    callsign=getenv("VCM_CALLSIGN") ? getenv("VCM_CALLSIGN") : "";
    if (control) {
        native_input=PV_GCS;
        native_wire=fopen(argv[5],"rb"); assert(native_wire);
        dvbtx_vcm_command(mixed ? "mixed" : "on");
    } else {
        char command[160]; int at=snprintf(command,sizeof command,"%u",hold);
        for (unsigned i=0;i<modes;++i) at+=snprintf(command+at,sizeof command-at," %u",ota ? (unsigned[]){4,11,13,17}[i] : first ? (unsigned[]){4,7,11}[i] : initial+i);
        dvbtx_vcm_command(command);
    }
    // Reference plain C and exported phases are checked independently in Python.
    phase_count=0;
    phases=malloc((2*frames+8)*DVBS2_MAX_SYMBOLS);
    FILE *expected=fopen(argv[4],"wb"); assert(expected);
    dvbs2_workspace ws;
    uint8_t payload[8100]; uint32_t seed=SEED;
    for (unsigned f=0;f<(control ? 2*frames+8 : frames+8);++f) {
        const unsigned mc=control ? (f ? target : initial) : ota ? (unsigned[]){4,11,13,17}[(f/hold)%modes] : first ? (unsigned[]){4,7,11}[(f/hold)%modes] : initial+(f/hold)%modes;
        const dvbs2_mode *mode=dvbs2_find_mode(mc);
        const size_t n=(mode->kbch-80)/8;
        for (size_t i=0;i<n;++i) payload[i]=next_byte(&seed);
        if (!control) station_payload_header(payload,n,callsign);
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
    assert(start_encoder("dvbdump",initial,128,0,sps,40000000));
    const int allocations=allocation_count;
    assert(vcm_enc.vcm_axes == (mixed && sps == 8));
    if (mixed) assert(!enc.block && dvb.ring.nslots >= 2);
    if (control && !mixed) assert(!vcm_control(initial == 12 ? 4 : 13));
    size_t internal=0;
    for (unsigned i=0;i<64;++i)
        if (native_blocks[i].p && (native_blocks[i].caps&MALLOC_CAP_INTERNAL)) internal+=native_blocks[i].n;
    if (!(sps == 8 && initial == 12)) assert(internal == ingress);
    printf("VCM n%u %s: internal run memory %zu, store %zu, table %zu, context %zu, slots %u\n",sps,argv[6],internal,
           vcm_enc.vcm_store_bytes,vcm_enc.vcm_axes ? SHAPER_AXES_BYTES : 4*shaper_table_words(sps,dvb.table.bits),dvbs2_fast_size(),dvb.ring.nslots);
    stop_encoder(); printf("}\n"); assert(run_returned());
    if (control) { rewind(native_wire); }
    // All startup allocations fail cleanly, without touching the radio.
    if (!control) for (int i=0;i<allocations;++i) {
        fail_after=i;
        assert(!start_encoder("dvbdump",initial,128,0,sps,40000000));
        assert(run_returned());
    }
    fail_after=-1;
    native_task_fail=1;
    assert(!start_encoder("dvbdump",initial,128,0,sps,40000000));
    assert(run_returned());
    native_task_fail=0;
    // Exercise the formerly overflowing command path, including staged stack
    // telemetry. The native high-water value is a stub, not a board measurement.
    if (control) rewind(native_wire);
    callsign=getenv("VCM_CALLSIGN") ? getenv("VCM_CALLSIGN") : "";
    for (unsigned repeat=0;repeat<2;++repeat) {
        if (control) rewind(native_wire);
        char iq[4096]; snprintf(iq,sizeof iq,"%s.iq",argv[3]);
        capture=fopen(iq,"wb"); assert(capture);
        dvbtx_dump(initial,frames,128,sps == 40 ? 10 : 1,0,sps,40000000);
        fclose(capture); assert(run_returned());
    }
    if (!control) dvbtx_pace(initial,1,128,0,sps,40000000,false);
    if (native_wire) fclose(native_wire);
    // Admission is explicit, including TS and high-rate rejection.
    native_input=PV_TS;
    assert(!start_encoder("dvbdump",initial,128,0,4,40000000));
    native_input=0;
    assert(!start_encoder("dvbdump",initial,128,0,4,80000000));
    dvbtx_vcm_command("4 4 13");
    assert(!start_encoder("dvbdump",4,128,0,4,40000000));
    dvbtx_vcm_command("on");
    // Internal run memory too small for the overlap cache: a clean, reported failure.
    native_internal_limit=native_internal_largest=SIZE_MAX;
    native_run[0].size=4096; native_run[1].size=0;
    assert(!start_encoder("dvbdump",13,128,0,8,40000000));
    assert(run_returned());
    native_run[0].size=RUN_LOW_BYTES; native_run[1].size=RUN_HIGH_BYTES;
    dvbtx_vcm_command("off");
    free(phases); free(banks);
    return 0;
}
