"""One-sweep correction against independent graph and full-decoder controls."""
from pathlib import Path
import json,os,subprocess,sys,tempfile,time,zlib
import numpy as np
P=Path(__file__).resolve().parent;R=P.parent
sys.path.insert(0,str(R/'tests'));import test_rx_mc6 as controls
import rx_mc6_vectors as oracle
sys.path.insert(0,str(R/'host'));import dvbs2 as d

def main():
    started=time.monotonic();report={'native_cycles':None,'hardware':False,'passed':False}
    with tempfile.TemporaryDirectory(prefix='mc6-first-') as td:
        exe=Path(td)/'worker';flags=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
          '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(R/'firmware/rx/fec'),'-I'+str(P),'-DRX_MC6_TEST_HOOKS=1']
        subprocess.run(flags+[str(P/'rx_mc6_first.c'),str(R/'firmware/rx/fec/rx_mc6_first.c'),str(R/'firmware/rx/fec/rx_mc6.c'),'-o',str(exe)],check=True)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
        count=0
        def run(llr,natural=False,wide=0):
            nonlocal count
            args=['state'] if natural else ['compare',str(wide)]
            data=(controls.graph_input()if natural else b'')+np.asarray(llr,dtype=np.int8).tobytes()
            p=subprocess.run([str(exe),*args],input=data,capture_output=True,check=True,env=env,timeout=120)
            result=json.loads(p.stderr);bits=np.unpackbits(np.frombuffer(p.stdout[:8100],np.uint8))
            assert result['H']==int(oracle.syndrome(bits).sum())
            assert len(p.stdout)==8100+result['bytes']
            count+=1
            return bits,p.stdout[8100:],result
        random=np.random.default_rng(0x6a8319).integers(-128,128,64800,dtype=np.int16)
        run(random,True)
        code,payload=controls.fixture(0x622026);run((1-2*code.astype(np.int16))*24,True)
        for wide in (1,2,3):run(np.zeros(64800,np.int8),wide=wide)
        native=[]
        for case,errors in enumerate((0,9,80)):
            contents=bytes((17*n+31*case+3)&255 for n in range(5370))
            word=d.fecframe(d.bbframe(np.unpackbits(np.frombuffer(contents,np.uint8)),'2/3'),'2/3')
            llr=np.where(word,-24,24).astype(np.int8)
            for n in range(errors):
                at=(7919*n+137*case)%64800;llr[at]=8 if llr[at]<0 else -8
            got,data,result=run(llr)
            assert result['valid'] and np.array_equal(got,word) and data==contents
            crc=zlib.crc32(np.packbits(got).tobytes());assert crc==(4240430337,2144775371,1400211940)[case]
            native.append(dict(case=case,flips=errors,word_crc=crc,result=result))
        external=0
        for word,plain in oracle.crosschecked_frames():
            got,data,result=run((1-2*word.astype(np.int16))*24)
            assert result['valid'] and np.array_equal(got,word) and data==np.packbits(plain[80:]).tobytes();external+=1
        outcomes=[]
        for seed in (1021,1022,1023):
            word,contents=controls.fixture(seed);signal=1-2*word.astype(np.int16);rng=np.random.default_rng(seed+9000)
            for sigma in (.2,.45,.7,1.):
                llr=np.clip(np.rint((signal+rng.normal(0,sigma,64800))*16),-31,31).astype(np.int8)
                got,data,result=run(llr)
                if result['valid']:assert np.array_equal(got,word) and data==contents
                else:assert not data and not result['bytes']
                outcomes.append(dict(seed=seed,sigma=sigma,**result))
        plain=d.bbframe(np.unpackbits(np.frombuffer(payload,np.uint8)),'2/3')
        headers=[d.bbheader(d.matype1(d.GCS),42968),d.bbheader(d.matype1(d.GCS),42960,upl=8),
                 d.bbheader(d.matype1(d.GCS),42960,syncd=8),d.bbheader(d.matype1(d.GCS),42960,mt2=1)]
        bad=plain[:80].copy();bad[79]^=1;headers.append(bad)
        for header in headers:
            x=plain.copy();x[:80]=header;word=d.fecframe(x,'2/3')
            got,data,result=run((1-2*word.astype(np.int16))*24)
            assert not result['valid'] and result['H']==0 and result['bch'] and not result['header'] and not data
        report.update(passed=True,whole_runs=count,per_tile_comparisons=count*2745,
          natural_graph_per_tile_comparisons=2*2745,full_S16_runs=3,native_fixtures=native,
          external_TS_words=external,malformed_BB_headers=5,one_sweep_heldouts=outcomes,
          resume_budgets=[0,1,3,4,89,90,134,2744,2745,7],host_seconds=time.monotonic()-started)
    print(json.dumps({k:report[k]for k in ('passed','whole_runs','per_tile_comparisons','host_seconds')}))
if __name__=='__main__':main()
