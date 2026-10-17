"""Real encoder/fill threads: deferred wakes and station-ID sample rebasing."""
import json
import os
import pathlib
import subprocess
import tempfile
import zlib
from test_stream import ROOT, compile_stream

gold = json.loads((ROOT/'tests/data/waveform_crcs.json').read_text())
gold.update(json.loads((ROOT/'tests/data/moon_stream_crcs.json').read_text()))
with tempfile.TemporaryDirectory() as tmp:
    folder = pathlib.Path(tmp)
    exe = compile_stream(folder)
    for mc, sps, rate, frames in ((13,4,40,40),(15,4,80,16),(11,12,80,16)):
        # N0CALL's measured 5.22 s opening ID, plus a short arbitrary offset.
        for shift in (4, (rate*5220000)%16380):
            path = folder/'dump.iq'
            result = subprocess.run([str(exe),str(mc),str(sps),str(path),'N0CALL',str(rate*1000000),str(frames)],
                                    env=dict(os.environ,BANK_SHIFT=str(shift),EARLY_WAKE='1'),
                                    capture_output=True,text=True,timeout=60)
            assert result.returncode == 0, result.stdout+result.stderr
            assert zlib.crc32(path.read_bytes()) == gold[f'{mc}/{sps}/{rate}/{frames}']
    print('wake: both callbacks, ID rebasing, early queue notifications, repeated starts/stops; 10/20/33m333 pass')
