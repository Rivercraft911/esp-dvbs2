"""Observed first-bank geometry, fast renderer path and every data word. No board."""
import argparse
import pathlib
import json
import os
import subprocess
import sys
import numpy as np
from test_stream import compile_stream

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'host'),str(ROOT/'tests/reference')]
import dvbs2
import lofty
import waveforms


def test(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='lofty_handoff.c')
    for mc in (13,15,17):
        rate, modulation = waveforms.mode(mc)
        bits = np.unpackbits(np.frombuffer(waveforms.payload(mc,1,'N0CALL'),np.uint8))
        fec = dvbs2.fecframe(dvbs2.bbframe(bits,rate),rate)
        phases = dvbs2.plframe_phases(fec,rate,modulation=modulation)
        for before in (None,1,2):
            name = f'{mc}-{before}'
            iq = out/f'{name}.iq'
            env = dict(os.environ)
            if before is not None: env['FORCE_DUMMY_FRAME'] = str(before)
            run = subprocess.run([str(exe),str(mc),str(iq)],env=env,capture_output=True,text=True,timeout=60)
            (out/f'{name}.log').write_text(run.stdout+run.stderr)
            assert run.returncode == 0,(name,run.stdout,run.stderr)
            if before is None:
                symbols = np.tile(phases,2)
            else:
                symbols = np.concatenate((np.tile(phases,before),np.tile(dvbs2.dummy_phases(),2),
                                          np.tile(phases,6-before)))
                rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
                late = next(r for r in rows if r.get('test') == 'transition' and r.get('first') == (417660772 if before == 1 else 417723732))
                assert late['dummy_before'] == 0 and late['dummy_after'] == (2 if before == 1 else 1),late
                assert late['singles'] <= 5 and late['switches'] == 1,late
                switches = [(r['relative'],r['kind']) for r in rows if r.get('test') == 'source']
                assert switches == [(before*66582,'dummy'),(before*66582+19980,'data')],switches
            expected = lofty.iq(lofty.shape(symbols))
            actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
            np.testing.assert_array_equal(actual,np.column_stack((expected.real,expected.imag)))
            print(name,'ID, EOF, dummy pair and return: every word exact; all slices reach bulk',flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    test(p.parse_args().output.resolve())
