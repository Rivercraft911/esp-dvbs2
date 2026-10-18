// Real transmitter lifecycle with the stream harness's peripheral stubs.
#define main fixed_stream_main
#include "stream_host.c"
#undef main

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
    const bool mixed=!strcmp(argv[6],"mixed"), first=!strcmp(argv[6],"first");
    const unsigned initial=!strcmp(argv[6],"8psk") ? 12 : 4;
    const unsigned modes=first ? 3 : mixed ? 14 : initial == 12 ? 6 : 8;
    const unsigned target=mixed ? 13 : initial == 12 ? 17 : 11;
    native_internal_limit=0; native_internal_largest=0;
    native_external_limit=1024*1024;
    if (sps == 8 && initial == 12) {
        native_internal_limit=16384; native_internal_largest=12288;
    }
    const unsigned frames=control ? 32 : 2*modes*hold;
    assert(!posix_memalign((void **)&banks,65536,2*sizeof *banks));
    bank_shift=37; // physical wrap and symbol cuts disagree at both 4/8 sps
    callsign="";
    if (control) {
        native_input=PV_GCS;
        native_wire=fopen(argv[5],"rb"); assert(native_wire);
        dvbtx_vcm_command(mixed ? "mixed" : "on");
    } else {
        char command[160]; int at=snprintf(command,sizeof command,"%u",hold);
        for (unsigned i=0;i<modes;++i) at+=snprintf(command+at,sizeof command-at," %u",first ? (unsigned[]){4,7,11}[i] : initial+i);
        dvbtx_vcm_command(command);
    }
    // Reference plain C and exported phases are checked independently in Python.
    phase_count=0;
    phases=malloc((2*frames+8)*DVBS2_MAX_SYMBOLS);
    FILE *expected=fopen(argv[4],"wb"); assert(expected);
    dvbs2_workspace ws;
    uint8_t payload[8100]; uint32_t seed=SEED;
    for (unsigned f=0;f<(control ? 2*frames+8 : frames+8);++f) {
        const unsigned mc=control ? (f ? target : initial) : first ? (unsigned[]){4,7,11}[(f/hold)%modes] : initial+(f/hold)%modes;
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
    assert(start_encoder("dvbdump",initial,128,0,sps,40000000));
    const int allocations=allocation_count;
    if (control && !mixed) assert(!vcm_control(initial == 12 ? 4 : 13));
    size_t internal=0;
    for (unsigned i=0;i<64;++i)
        if (native_blocks[i].p && (native_blocks[i].caps&MALLOC_CAP_INTERNAL)) internal+=native_blocks[i].n;
    if (!(sps == 8 && initial == 12)) assert(internal == 0);
    printf("VCM n%u %s: internal heap %zu, store %zu, table %zu, context %zu, slots %u\n",sps,argv[6],internal,
           enc.vcm_store_bytes,4*shaper_table_words(sps,dvb.table.bits),dvbs2_fast_size(),dvb.ring.nslots);
    stop_encoder(); printf("}\n"); assert(!live_allocations);
    if (control) { rewind(native_wire); }
    // All startup allocations fail cleanly, without touching the radio.
    if (!control) for (int i=0;i<allocations;++i) {
        fail_after=i;
        assert(!start_encoder("dvbdump",initial,128,0,sps,40000000));
        assert(!live_allocations);
    }
    fail_after=-1;
    native_task_fail=1;
    assert(!start_encoder("dvbdump",initial,128,0,sps,40000000));
    assert(!live_allocations);
    native_task_fail=0;
    for (unsigned repeat=0;repeat<2;++repeat) {
        if (control) rewind(native_wire);
        char iq[4096]; snprintf(iq,sizeof iq,"%s.iq",argv[3]);
        capture=fopen(iq,"wb"); assert(capture);
        dvbtx_dump(initial,frames,128,sps == 40 ? 10 : 1,0,sps,40000000);
        fclose(capture); assert(!live_allocations);
    }
    if (native_wire) fclose(native_wire);
    // Admission is explicit, including TS and high-rate rejection.
    native_input=PV_TS;
    assert(!start_encoder("dvbdump",initial,128,0,4,40000000));
    native_input=0;
    assert(!start_encoder("dvbdump",initial,128,0,4,80000000));
    dvbtx_vcm_command("4 4 13");
    assert(!start_encoder("dvbdump",4,128,0,8,40000000));
    dvbtx_vcm_command("on");
    native_internal_limit=16384; native_internal_largest=4096;
    assert(!start_encoder("dvbdump",13,128,0,8,40000000));
    assert(!live_allocations);
    dvbtx_vcm_command("off");
    free(phases); free(banks);
    return 0;
}
