"""Observed first-bank geometry, fast renderer path and every data word. No board."""
import argparse
import pathlib
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
        iq = out/f'{mc}.iq'
        run = subprocess.run([str(exe),str(mc),str(iq)],capture_output=True,text=True,timeout=60)
        (out/f'{mc}.log').write_text(run.stdout+run.stderr)
        assert run.returncode == 0,(mc,run.stdout,run.stderr)
        rate, modulation = waveforms.mode(mc)
        bits = np.unpackbits(np.frombuffer(waveforms.payload(mc,1,'N0CALL'),np.uint8))
        fec = dvbs2.fecframe(dvbs2.bbframe(bits,rate),rate)
        phases = dvbs2.plframe_phases(fec,rate,modulation=modulation)
        expected = lofty.iq(lofty.shape(np.tile(phases,2)))
        actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
        np.testing.assert_array_equal(actual,np.column_stack((expected.real,expected.imag)))
        print(mc,'ID handoff: 8100+6092 samples, 1182 bulk blocks, one tail; every word exact',flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    test(p.parse_args().output.resolve())
