"""Forced standard dummies through the real fill/overlap path. No board."""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import numpy as np
from test_stream import compile_stream

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'host'), str(ROOT/'tests/reference')]
import lofty
import waveforms


def test(out, receiver):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out)
    for mc in (13,15,17):
        iq = out/f'{mc}.iq'
        env = dict(os.environ,FORCE_DUMMIES='1',BANK_SHIFT='1540')
        r = subprocess.run([str(exe),str(mc),'3',str(iq),'N0CALL','80000000','16'],
                           env=env,capture_output=True,text=True,timeout=90)
        (out/f'{mc}.log').write_text(r.stdout+r.stderr)
        assert r.returncode == 0,(mc,r.stderr)
        rows = [json.loads(l[1:]) for l in r.stdout.splitlines() if l.startswith('@{')]
        dump, = [row for row in rows if 'crc32' in row][:1]
        assert dump['dummy_startup'] == 4 and dump['dummy_steady'] == 2 and dump['dummy_alignment'] == 3
        assert dump['queue_empty'] == dump['underruns'] == dump['overlap_mismatches'] == 0
        s = waveforms.symbols(mc,17,dummy_before=(0,0,1,1,5,5),callsign='N0CALL')
        phases = np.rint(np.angle(s)*4/np.pi).astype(int)%8
        expected = lofty.iq(lofty.shape(phases[:16*22194]))
        actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
        np.testing.assert_array_equal(actual,np.column_stack((expected.real,expected.imag)))
        print(mc,'six dummies, every DAC word equals Python, startup=4 steady=2',flush=True)
        if receiver:
            cmd = [sys.executable,str(ROOT/'host/decode.py'),str(iq),'--rate','80000000',
                   '--symbol-rate',str(80000000/3),'--rolloff','.35','--modcod',str(mc),
                   '--scale','512','--stream','16','--callsign','N0CALL','--receiver',str(receiver)]
            run = subprocess.run(cmd,capture_output=True,text=True,timeout=120)
            (out/f'{mc}.decode.json').write_text(run.stdout+run.stderr)
            assert run.returncode == 0,(mc,run.stdout,run.stderr)
            d = json.loads(run.stdout)
            assert d['frame_lock'] and d['status_complete'] and d['payload_match']
            assert d['first_frame'] in (0,1) and d['payload_frames'] >= 12
            assert all(d[k] == 0 for k in ('bch_failed_frames','bch_corrected_bits',
                       'ldpc_checks_before','ldpc_bit_flips','payload_bit_errors')),d
            print(mc,'LeanDVB:',d['payload_frames'],'contiguous exact data frames',flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    p.add_argument('--receiver',type=pathlib.Path)
    a = p.parse_args()
    test(a.output.resolve(),a.receiver)
