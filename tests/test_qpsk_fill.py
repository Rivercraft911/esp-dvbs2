"""Independent table/convolution checks, native C and optional QEMU records. No devices."""
import json
import pathlib
import subprocess
import sys
import tempfile
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import qpsk_fill as q
import shaper as s


def check_records(text):
    rows = [json.loads(line[1:]) for line in text.splitlines() if line.startswith('@{"test":"qpsk_fill')]
    assert len(rows) == 19, len(rows)
    seen = set()
    for r in rows[:-1]:
        n, level, seed = r['sps'], r['level'], r['seed']
        seen.add((n,level,seed))
        assert r['errors'] == 0, r
        x = (2026,7,0xffffffff)[seed]
        src = []
        for _ in range(2+16384//(4*n)):
            x ^= x << 13 & 0xffffffff
            x ^= x >> 17
            x ^= x << 5 & 0xffffffff
            src.append((x >> 16) & 255)
        assert r['table_check'] == s.checksum(q.table(n,level).ravel()),r
        assert r['check'] == s.checksum(q.shape(np.array(src,np.uint8),n,level)),r
    assert seen == {(n,l,z) for n in (3,4) for l in (16,128,508) for z in range(3)}
    assert rows[-1]['errors'] == 0 and rows[-1]['cases'] == 19368, rows[-1]
    print('QPSK C/PIE: 19368 guarded sample slices, all tables/checks match Python')


def test_model():
    assert q.HEADER.read_text() == q.header_text()
    rng = np.random.default_rng(2026)
    for n in (3,4):
        for level in (16,128,508):
            r = q.parts(n,level)
            # Exhaustive independent extrema across all 256^3 combinations, factored by part.
            assert (r.min(axis=2).sum(axis=1) >= -level).all()
            assert (r.max(axis=2).sum(axis=1) <= level).all()
            phases = rng.choice([1,3,5,7], size=8+4*257)
            src = q.pack(phases)
            assert np.array_equal((src[:,None] >> np.array([6,4,2,0]) & 3)*2+1,
                                  phases.reshape(-1,4))
            words = q.shape(src,n,level)
            linear = np.zeros((2,len(words)),np.int64)
            taps = q.pulse(n,level)
            for axis, levels in enumerate((s.LEVEL_I,s.LEVEL_Q)):
                for age in range(9):
                    for p in range(n):
                        linear[axis,p::n] += levels[phases[8-age:len(phases)-age]]*taps[age*n+p]
            exact = linear.astype(float)/2**s.SHIFT
            got = s.iq(words)
            assert max(abs(got.real-exact[0]).max(),abs(got.imag-exact[1]).max()) <= 1.5
        margin = s.mask_margins(q.pulse(n),n,.35)
        assert min(margin[k] for k in ('upper','lower','far')) > 0, margin
    print('QPSK model: 9-symbol convolution within 1.5 LSB/axis; full table extrema fit, pulse mask passes')


def test_geometry():
    assert q.geometry(0) == dict(fresh=[(0,16380)],reused=0,saved=768)
    assert q.geometry(1)['reused'] == 768
    for n in (3,4):
        for bank in range(2,3278):
            g = q.geometry(bank)
            assert sum(count for _,count in g['fresh']) == 15740
            assert g['reused'] == 640 and g['saved'] <= 1536
            assert all(at%4 == count%4 == 0 for at,count in g['fresh'])
    print('QPSK geometry: all bank/block phases, first-bank extra lead, at most two cache saves')


def test_native():
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        (folder/'esp_attr.h').write_text('#define IRAM_ATTR\n')
        exe = folder/'qpsk'
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DQPSK_HOST',f'-I{tmp}',
                        f'-I{ROOT}/firmware/main',str(ROOT/'tests/qpsk_fill.c'),
                        str(ROOT/'firmware/main/shaper_qpsk.c'),str(ROOT/'firmware/main/ring.c'),'-o',str(exe)],check=True)
        check_records(subprocess.check_output([str(exe)],text=True))


if __name__ == '__main__':
    test_model()
    test_geometry()
    test_native()
    if len(sys.argv)>1: check_records(pathlib.Path(sys.argv[1]).read_text())
