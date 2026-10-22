"""Independent Q32/Q15 equations and normal/pilot MC4/MC6 framing controls."""
from pathlib import Path
import json
import math
import subprocess
import sys
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2

DRIVER = r'''
#include "rx_carrier.c"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int16_t frame[RX_CARRIER_SYMBOLS][2], derivative[RX_CARRIER_KNOWN][2];
static int8_t llr[RX_CARRIER_BITS];
static uint8_t word[RX_CARRIER_WORD_BYTES], gold[RX_CARRIER_GOLD_BYTES];
static rx_carrier state;
static void write_file(const char *prefix, const char *suffix, const void *data, size_t bytes)
{
    char path[1024]; assert(snprintf(path,sizeof path,"%s.%s",prefix,suffix)>0);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(data,1,bytes,f)==bytes); fclose(f);
}
int main(int argc,char **argv)
{
    assert(argc==4);
    if (!strcmp(argv[1], "wedge")) {
        FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
        assert(in && out);
        uint32_t pair[2];
        while (fread(pair, sizeof pair, 1, in) == 1) {
            float r, i; memcpy(&r, pair, 4); memcpy(&i, pair+1, 4);
            uint8_t value = pilot_error(r, i);
            assert(fwrite(&value, 1, 1, out) == 1);
        }
        fclose(in); fclose(out); return 0;
    }
    rx_carrier_gold(gold);
    write_file(argv[3],"gold",gold,sizeof gold);
    int8_t header[90][2]; assert(rx_carrier_header((unsigned)atoi(argv[2]),header));
    write_file(argv[3],"header",header,sizeof header);
    int16_t phasors[4096*3][2], rotations[4096*3][2];
    const int16_t samples[4][2]={{-32768,-32768},{32767,-32768},{0,0},{-3,2}};
    for(unsigned n=0;n<4096*3;++n) {
        uint32_t p=(n/3)*(UINT32_C(1)<<20)+(UINT32_C(1)<<19)+(n%3)-1;
        rx_carrier_phasor(p,phasors[n]);
        rx_carrier_rotate(samples[n%4],p,rotations[n]);
    }
    write_file(argv[3],"phasor",phasors,sizeof phasors);
    write_file(argv[3],"rotation",rotations,sizeof rotations);
    FILE *f=fopen(argv[1],"rb"); assert(f && fread(frame,1,sizeof frame,f)==sizeof frame); fclose(f);
    rx_carrier_metadata m={.first_native_sample=UINT64_C(0x123456789abcdef0),
        .input_origin=UINT64_C(0x10000000000),.input_step_q32=UINT64_C(4)<<32,
        .input_fraction=UINT32_MAX,.symbol_rate_hz=1000000,.modcod=(unsigned)atoi(argv[2])};
    memset(&state,0x6d,sizeof state);rx_carrier before=state;
    assert(!rx_carrier_estimate(&state,NULL,NULL,gold,&m));
    assert(!memcmp(&state,&before,sizeof state));
    assert(!rx_carrier_header(5,header));
    assert(rx_carrier_estimate(&state,frame,NULL,gold,&m));
    assert(!state.timing_observed);
    rx_carrier prepared=state;
    write_file(argv[3],"base",state.base,sizeof state.base);
    write_file(argv[3],"step",state.step,sizeof state.step);
    int16_t cuts[]={state.high,state.negative_high,state.low};
    write_file(argv[3],"cuts",cuts,sizeof cuts);
    memset(llr,0x57,sizeof llr);memset(word,0x6d,sizeof word);
    assert(!rx_carrier_project(&state,frame,gold,0,llr,word));
    assert(!rx_carrier_project(&state,frame,gold,65,llr,word));
    assert(!rx_carrier_project(&state,frame,gold,64,llr,(uint8_t *)llr));
    assert(!memcmp(&state,&prepared,sizeof state));
    unsigned call=0;
    while(state.cursor<RX_CARRIER_SYMBOLS) {
        unsigned n=1+(call++*17)%64;
        assert(rx_carrier_project(&state,frame,gold,n,llr,word));
        assert(state.cursor<=RX_CARRIER_SYMBOLS && state.emitted<=RX_CARRIER_BITS);
    }
    assert(state.emitted==RX_CARRIER_BITS && !state.failed);
    assert(state.metadata.first_native_sample==m.first_native_sample &&
           state.metadata.input_origin==m.input_origin && state.metadata.input_fraction==m.input_fraction);
    write_file(argv[3],"llr",llr,sizeof llr);write_file(argv[3],"word",word,sizeof word);
    int8_t saved_llr[RX_CARRIER_BITS];uint8_t saved_word[RX_CARRIER_WORD_BYTES];
    memcpy(saved_llr,llr,sizeof llr);memcpy(saved_word,word,sizeof word);
    state=prepared;
    while(state.cursor<RX_CARRIER_SYMBOLS)assert(rx_carrier_project(&state,frame,gold,64,llr,word));
    assert(!memcmp(saved_llr,llr,sizeof llr) && !memcmp(saved_word,word,sizeof word));
    assert(rx_carrier_estimate(&before,frame,derivative,gold,&m));
    assert(before.timing_observed==UINT32_C(0x7fffff));
    for(unsigned k=0;k<23;++k)assert(before.timing_gradient[k]==0);
    for(unsigned k=0;k<RX_CARRIER_KNOWN;++k)
        for(unsigned a=0;a<2;++a)derivative[k][a]=(int16_t)((13*k+7*a)%97)-48;
    assert(rx_carrier_estimate(&before,frame,derivative,gold,&m));
    assert(before.timing_observed==UINT32_C(0x7fffff));
    write_file(argv[3],"gradient",before.timing_gradient,sizeof before.timing_gradient);
    write_file(argv[3],"coherence",before.timing_coherence,sizeof before.timing_coherence);
    printf("{\"modcod\":%u,\"header_errors\":%u,\"pilot_errors\":%u,\"amplitude\":%.9g,\"slope\":%.9g,\"state_bytes\":%zu}\n",
        m.modcod,state.header_errors,state.pilot_errors,state.amplitude,state.slope,sizeof state);
    return 0;
}
'''


def main():
    rng = np.random.default_rng(20261022)
    bits = rng.integers(0, 2, 64800, dtype=np.uint8)
    phases = ((np.arange(4096 * 3, dtype=np.uint64)//3)*(1 << 20) +
              (1 << 19)+(np.arange(4096 * 3, dtype=np.uint64)%3)-1) & 0xffffffff
    index = ((phases+(1 << 19)) & 0xffffffff) >> 20
    phi = index * (2*math.pi/4096)
    oracle = np.rint(32767*np.stack((np.cos(phi), np.sin(phi)), axis=1)).astype(np.int16)
    samples = np.array([[-32768,-32768],[32767,-32768],[0,0],[-3,2]], np.int64)
    x = samples[np.arange(len(phases)) % 4]
    c = oracle.astype(np.int64)
    rotated = np.stack((x[:,0]*c[:,0]-x[:,1]*c[:,1],
                        x[:,0]*c[:,1]+x[:,1]*c[:,0]), axis=1)//65536
    with tempfile.TemporaryDirectory(prefix="rx-carrier-") as tmp:
        p = Path(tmp); source=p/"driver.c"; source.write_text(DRIVER); exe=p/"driver"
        subprocess.run(["cc","-std=c11","-O2","-Wall","-Wextra","-Werror",
                        "-ffp-contract=off","-fno-fast-math","-fsanitize=address,undefined",
                        "-I"+str(ROOT/"firmware/rx/dsp"),str(source),
                        "-lm","-o",str(exe)],check=True)
        # Binary32 inputs checked with exact binary64 halfspaces. No angles
        # are used for finite inputs, including near-diagonal ties.
        pairs = []
        for exponent in range(1, 255):
            for mantissa in (0, 0x400000, 0x7ffffb):
                real = (exponent << 23) | mantissa
                for offset in range(-4, 5):
                    imag = max(0, min(0x7f7fffff, real+offset))
                    for sr in (0, 0x80000000):
                        for si in (0, 0x80000000):
                            pairs.append((real | sr, imag | si))
        special = (0, 0x80000000, 1, 0x80000001, 0x7fffff, 0x807fffff,
                   0x800000, 0x80800000, 0x3f800000, 0xbf800000,
                   0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000)
        pairs.extend((a, b) for a in special for b in special)
        raw = np.asarray(pairs, dtype='<u4'); raw.tofile(p/'wedges')
        real, imag = raw.view('<f4').astype(np.float64).T
        with np.errstate(invalid='ignore'):
            expected = (real+imag < 0) | (real-imag < 0)
        zero = (real == 0) & (imag == 0)
        expected[zero] = (raw[zero, 0] >> 31).astype(bool)
        finite = np.isfinite(real) & np.isfinite(imag)
        with np.errstate(invalid='ignore'):
            expected[~finite] = np.abs(np.arctan2(imag[~finite], real[~finite])) > np.float32(np.pi/4)
        subprocess.run([str(exe), 'wedge', str(p/'wedges'), str(p/'wedge-out')], check=True)
        assert np.array_equal(expected, np.fromfile(p/'wedge-out', np.uint8))
        print(f"Carrier wedge: {len(raw)} independent boundary controls PASS", flush=True)
        for case, (modcod, rate, cfo, initial) in enumerate([
                (4,"1/2",-12000,-2.7),(4,"1/2",12000,2.9),
                (6,"2/3",-12000,1.4),(6,"2/3",12000,-.4)]):
            i,q = dvbs2.plframe(bits,rate)
            n=np.arange(len(i)); angle=initial+2*math.pi*cfo*n/1e6+.08*np.sin(2*math.pi*n/20000)
            z=(1-2*i.astype(float)+1j*(1-2*q.astype(float)))*30*np.exp(1j*angle)
            # Independent high-SNR noise and whole-frame input quantization.
            z += .15*(rng.normal(size=len(n))+1j*rng.normal(size=len(n)))
            frame=np.rint(64*np.stack((z.real,z.imag),axis=1)).astype(np.int16)
            record=p/f"frame{case}";frame.tofile(record); out=p/f"out{case}"
            run=subprocess.run([str(exe),str(record),str(modcod),str(out)],check=True,capture_output=True,text=True)
            stats=json.loads(run.stdout);assert stats["header_errors"]==stats["pilot_errors"]==0
            assert out.with_suffix(".word").read_bytes()==np.packbits(bits).tobytes()
            # Evaluate actual emitted coefficients independently using int64
            # products, NumPy floor, and the stated asymmetric integer cuts.
            base=np.fromfile(out.with_suffix(".base"),np.uint32).astype(np.uint64)
            step=np.fromfile(out.with_suffix(".step"),np.uint32).astype(np.uint64)
            cuts=np.fromfile(out.with_suffix(".cuts"),np.int16).astype(int)
            anchors=3095+2952*np.arange(22)
            segment=np.minimum(np.searchsorted(anchors,2*n,side="left"),21)
            phase=(base[segment]+n.astype(np.uint64)*step[segment]) & 0xffffffff
            original=phase.copy()
            r=dvbs2.pl_scrambling_sequence(33192).astype(np.uint64)
            phase[90:]=(phase[90:]-(r<<30)) & 0xffffffff
            pi=(((phase+(1<<19)) & 0xffffffff)>>20).astype(int)
            ph=np.rint(32767*np.stack((np.cos(2*np.pi*pi/4096),np.sin(2*np.pi*pi/4096)),axis=1)).astype(np.int64)
            x=frame.astype(np.int64)
            projected=np.stack((x[:,0]*ph[:,0]-x[:,1]*ph[:,1],x[:,0]*ph[:,1]+x[:,1]*ph[:,0]),axis=1)//65536
            data=(n>=90) & ~((n>=1530) & ((n-1530)%1476<36))
            values=projected[data].ravel()
            expected=np.where(values>=cuts[0],24,np.where(values<=cuts[1],-24,np.where(values<cuts[2],-8,8))).astype(np.int8)
            assert np.array_equal(expected,np.fromfile(out.with_suffix(".llr"),np.int8))
            error=np.angle(np.exp(1j*(original.astype(float)*(2*np.pi/2**32)+angle)))
            assert np.max(abs(error))<.03, "independent high-SNR generated-phase gate"
            # Sparse derivative observations, independent double-precision
            # normalized-coherence equation; production reductions are float.
            slope=float(np.float32(stats["slope"]))
            coarse=int(math.floor(((-slope/(2*math.pi))%1)*2**32+.5)) & 0xffffffff
            gradient=[];coherence=[]
            reference_i,reference_q=dvbs2.plheader(modcod,False,True)
            header_quarter=np.where(reference_i==0,np.where(reference_q==0,0,3),np.where(reference_q==0,1,2))
            derivatives=((13*np.arange(882)[:,None]+7*np.arange(2))%97-48).astype(np.int64)
            for block in range(23):
                first=1530+1476*(block-1) if block else 0
                count=36 if block else 90
                at=np.arange(first,first+count,dtype=np.uint64)
                rq=r[first-90:first-90+count] if block else header_quarter.astype(np.uint64)
                phase=(coarse*at-(rq<<30)-(1<<29)) & 0xffffffff
                pi=(((phase+(1<<19)) & 0xffffffff)>>20).astype(int)
                ph=np.rint(32767*np.stack((np.cos(2*np.pi*pi/4096),np.sin(2*np.pi*pi/4096)),axis=1)).astype(np.int64)
                start=90+36*(block-1) if block else 0
                def rotate(v):
                    return ((v[:,0]*ph[:,0]-v[:,1]*ph[:,1])+1j*(v[:,0]*ph[:,1]+v[:,1]*ph[:,0]))/2**21
                z=rotate(frame[first:first+count].astype(np.int64));dz=rotate(derivatives[start:start+count])
                S=z.sum();D=dz.sum();E=np.sum(abs(z)**2);dE=2*np.vdot(z,dz).real
                coherence.append(abs(S)**2/(count*E))
                gradient.append((2*(S.conjugate()*D).real-abs(S)**2*dE/E)/(count*E))
            assert np.max(abs(np.fromfile(out.with_suffix(".gradient"),np.float32)-gradient))<1e-5
            assert np.max(abs(np.fromfile(out.with_suffix(".coherence"),np.float32)-coherence))<1e-5
            gold=np.frombuffer(out.with_suffix(".gold").read_bytes(),np.uint8)
            unpacked=((gold[:,None] >> (2*np.arange(4)))&3).ravel()
            assert np.array_equal(unpacked,dvbs2.pl_scrambling_sequence(33192))
            hi,hq=dvbs2.plheader(modcod,False,True)
            assert np.array_equal(np.fromfile(out.with_suffix(".header"),np.int8).reshape(-1,2),
                                  1-2*np.stack((hi,hq),axis=1).astype(np.int8))
            assert np.array_equal(np.fromfile(out.with_suffix(".phasor"),np.int16).reshape(-1,2),oracle)
            assert np.array_equal(np.fromfile(out.with_suffix(".rotation"),np.int16).reshape(-1,2),rotated)
            print(json.dumps(stats,sort_keys=True))
    print("RX carrier: 12288 phase boundaries/extremes; 4 complete MC4/MC6 frames; chunk/ownership controls PASS (host only)")


if __name__ == "__main__":
    main()
