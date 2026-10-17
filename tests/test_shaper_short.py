"""Independent waveform/table checks; optional QEMU or board records, never serial."""
import json
import pathlib
import subprocess
import sys
import tempfile
import numpy as np
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'host'))
import shaper as s


# Independently derive the integer pulse; firmware retains the measured constants.
g = s.corrected_rrc(.35, 4, span=6)
PULSE4 = np.round(g * 508 / abs(g.reshape(6, 4)).sum(0).max() * 256).astype('int64')

def profile(peak=128):
    p = s.Profile('short', .35, 4, '8psk', span=6)
    p.__dict__['taps'] = np.trunc(PULSE4 * peak / 718.4).astype('int64')
    return p


def check(rows):
    assert len(rows) == 162, len(rows)
    for r in rows:
        p = profile(r['peak'])
        assert r['mismatch'] == 0 and r['guards'] == 1, r
        assert r['table'] == s.checksum(p.table.ravel()), r
        sym = s.random_symbols(p, r['symbols']+8, r['seed'])
        assert r['check'] == s.checksum(s.shape(p, sym[3:])), r


def main():
    for level in (16,128,508):
        p = profile(level)
        for axis in range(2):
            values = p.parts[axis] + p.biases[axis][:,None,:]
            assert values.min() >= 0 and values.max(1).sum(0).max() < 1024
    with tempfile.TemporaryDirectory() as tmp:
        pathlib.Path(tmp,'esp_attr.h').write_text('#define IRAM_ATTR\n')
        exe = pathlib.Path(tmp,'short')
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DSHORT_HOST','-I'+tmp,
                        '-I'+str(ROOT/'firmware/main'), str(ROOT/'tests/shaper_short.c'),
                        str(ROOT/'firmware/main/shaper.c'),str(ROOT/'firmware/main/shaper_short.c'),
                        '-o',str(exe)], check=True)
        records = [json.loads(x[1:]) for x in subprocess.check_output([str(exe)],text=True).splitlines()]
        check(records)
    if len(sys.argv)>1:
        rows = [json.loads(x[1:]) for x in pathlib.Path(sys.argv[1]).read_text().splitlines()
                if x.startswith('@{"test":"short"')]
        check(rows)
        assert rows == records
    print('short: 162 cases, independent pulse/tables, bounds, C/Python and optional PIE agree')
if __name__ == '__main__': main()
