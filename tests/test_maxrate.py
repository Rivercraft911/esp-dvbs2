"""Independent table, output, whole-transmitter budget and optional PIE checks."""
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
import maxrate as m
import shaper as s


def check(records):
    table = m.tables()
    assert len(records) == 24
    for row in records:
        assert row['mismatch'] == 0 and row['guards'] == 1, row
        assert row['table'] == [s.checksum(t.ravel()) for t in table], row
        p = s.Profile('n3', .35, 3, '8psk')
        phases = s.random_symbols(p, 8 + row['groups']*4, row['seed']) >> 4
        assert row['check'] == s.checksum(m.shape(phases)), row


def test_pulse():
    g = s.corrected_rrc(.35, 3, span=6)
    unscaled = np.round(g * 508 / abs(g.reshape(6, 3)).sum(0).max() * 256).astype('int64')
    assert np.array_equal(unscaled, m.PULSE)
    # Every independent table field and their worst-case sum: no inter-field carry.
    t = m.tables().astype('int64')
    for fields in (t & 1023, t >> 10):
        assert fields.min() >= 0 and fields.max(1).sum(0).max() <= 1023
    # Equal I/Q means after de-biasing; the rounding cannot introduce a DC line.
    assert not m.parts().sum(2).any()
    p = s.Profile('n3', .35, 3, '8psk')
    phases = s.random_symbols(p, 4008, 7) >> 4
    y = s.iq(m.shape(phases))
    z = (s.LEVEL_I[phases] + 1j*s.LEVEL_Q[phases]) / 2**30
    up = np.zeros(3*len(z), complex)
    up[::3] = z
    linear = np.convolve(up, m.taps()/256)[24:3*len(z)]
    assert max(abs(y.real-linear.real).max(), abs(y.imag-linear.imag).max()) <= 1.5
    # Constant limiting cases also exercise zero coefficients at the ends of each part.
    for phase in range(8):
        y = s.iq(m.shape(np.full(24, phase)))
        assert np.array_equal(y[:12], y[12:24])


def test_budgets():
    b = m.budget(60700)
    assert b['frame_cycles'] == 199746 and b['bank_cycles'] == 42804
    assert b['two_core_cycles_per_frame'] > b['two_core_capacity']
    assert m.budget(0, encoder=0)['two_core_margin'] == 1
    assert m.budget(42804, encoder=199746)['two_core_margin'] == 0
    assert m.budget(60700, fs=40e6)['fixed_split_margin'] > .15
    # The reserve belongs to both cores, including the measured encoder.
    b = m.budget(30000)
    assert abs(m.budget(30000, encoder=b['max_encoder_15pct'])['two_core_margin'] - .15) < 1e-12


def check_board(path):
    expected = {f'block4_table{i}':s.checksum(t.ravel()) for i,t in enumerate(m.tables())}
    p = s.Profile('n3', .35, 3, '8psk')
    phases = s.random_symbols(p, 5468, 2026) >> 4
    expected['block4_c'] = expected['block4_pie'] = s.checksum(m.shape(phases))
    seen = set()
    for line in path.read_text().splitlines():
        row = json.loads(line.removeprefix('@'))
        if row.get('cmd') == 'membench': assert 'error' not in row, row
        if row.get('test') in expected:
            assert row['check'] == expected[row['test']], row
            seen.add(row['test'])
    assert seen == expected.keys(), (seen, expected)


if __name__ == '__main__':
    test_pulse()
    test_budgets()
    with tempfile.TemporaryDirectory() as tmp:
        pathlib.Path(tmp, 'esp_attr.h').write_text('#define IRAM_ATTR\n')
        exe = pathlib.Path(tmp, 'maxrate')
        main = ROOT / 'firmware/main'
        subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DMAXRATE_HOST',
                        '-I'+tmp, '-I'+str(main), str(ROOT/'tests/maxrate.c'),
                        str(main/'shaper.c'), str(main/'shaper_block.c'), '-o', str(exe)], check=True)
        out = subprocess.check_output([str(exe)], text=True)
        records = [json.loads(line[1:]) for line in out.splitlines()]
        check(records)
        if len(sys.argv) > 1:
            if sys.argv[1] == '--membench':
                check_board(pathlib.Path(sys.argv[2]))
                print('block4 membench: all five checks match the independent model')
                sys.exit(0)
            qemu = [json.loads(line[1:]) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
                    if line.startswith('@{"test":"maxrate"')]
            check(qemu)
            assert records == qemu
    print('maxrate: pulse, bounds, 24 C/Python cases and optional PIE output agree')
