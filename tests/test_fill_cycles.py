"""Every bank-wrap phase, including the initial extra lead. No timing or hardware."""
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'host'))
import fill_cycles as f

assert f.geometry(0) == dict(start=0, fresh=[(0, 16380)], reused=0, saved=2176)
assert sum(n for _, n in f.geometry(1)['fresh']) == f.STEP - f.LEAD
assert f.geometry(1)['reused'] == f.CACHE
for b in range(2, f.BANK // math.gcd(f.BANK, f.STEP) + 2):
    g = f.geometry(b)
    assert g['reused'] == 2112
    assert sum(n for _, n in g['fresh']) == f.STEP
    assert 2176 <= g['saved'] <= 4352  # two output pieces can each save a full tail
    assert all(at % 4 == n % 4 == 0 for at, n in g['fresh'])
    assert all(at // f.BANK == (at + n - 1) // f.BANK for at, n in g['fresh'])
print('fill geometry: every wrap, 14268 fresh words, 2112 reused, startup extra 64, save cost retained')
