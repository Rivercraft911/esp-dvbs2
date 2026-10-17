"""8PSK transmitter model, synthetic MER and optional independent receiver loopback.

python3 tests/test_tx_8psk.py [--loopback --receiver /path/to/leandvb --out runs.jsonl]
All waveforms are host generated. No board, serial port, network or RF access.
"""
import argparse
import ctypes as ct
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'host'), str(ROOT / 'tests')]
import dvbs2
import iq
import onair
import shaper
from test_dvbs2_c import NativeEncoder


def waveform(mc, n, frames, dummies=()):
    phases = (np.rint(np.angle(onair.symbols(mc, frames, dummies)) * 4 / np.pi).astype(int) % 8)
    p = shaper.Profile('scaled', .35, n, '8psk')
    base = shaper.PROFILES[f'8psk-n{n}-a35']
    # dvbtx.c truncates integer taps after multiplying by peak_level/WORST_PEAK.
    p.__dict__['taps'] = np.trunc(base.taps * 128 / 718.4).astype(np.int64)
    return shaper.iq(shaper.shape(p, np.r_[np.zeros(8, np.uint8), phases << 4]))


def test_dummy():
    with NativeEncoder() as c:
        c.lib.dvbs2_dummy.argtypes = [ct.POINTER(ct.c_uint8)]
        out = np.full(3332, 0xee, np.uint8)
        c.lib.dvbs2_dummy(out.ctypes.data_as(ct.POINTER(ct.c_uint8)))
        assert np.array_equal(out[:3330], dvbs2.dummy_phases())
        assert np.all(out[3330:] == 0xee)
    assert np.all((dvbs2.dummy_phases()[90:] - 2 * dvbs2.pl_scrambling_sequence(3240, 0)) % 8 == 1)
    print('dummy: C equals independent standard-formula Python, 3330 phases, output guards intact')


def test_mer():
    for mc, n in ((13, 8), (15, 4)):
        rs = 40e6 / n
        # Arbitrary stream interior, carrier offset, +73 ppm, independently shaped RRC.
        phases = np.rint(np.angle(onair.symbols(mc, 3)) * 4 / np.pi).astype(np.int64) % 8
        z = iq.shape(phases.astype(np.uint8), rs, 40e6, span=32, snr_db=30 - 10*np.log10(n),
                     frequency_offset=.011 * rs, sample_rate_offset_ppm=73, seed=20261016)
        r = onair.recover_mer(z[12345:-8000], 40e6, rs, mc, frames=3)
        assert abs(r['mer_db'] - 30) < .4, r
        assert abs(r['sample_clock_offset_ppm'] - 73) < 2, r
        assert abs(r['carrier_offset_hz'] - .011 * rs / 1.000073) < 20, r
        print('synthetic:', json.dumps({'modcod': mc, 'sps': n, **r}))
        # Quantized, scaled transmitter table with two consecutive dummies.
        r = onair.recover_mer(waveform(mc, n, 3, (1, 1)), 40e6, rs, mc, shift=0,
                              frames=3, dummy_before=(1, 1))
        assert r['mer_db'] > 38, r
        print('table model:', json.dumps({'modcod': mc, 'sps': n, **r}))


def loopback(receiver, output):
    for mc in (13, 15):
        for n in (4, 8):
            for dummies in ((), (8, 8, 9)):
                with tempfile.TemporaryDirectory() as tmp:
                    path = pathlib.Path(tmp) / 'tx.iq'
                    z = waveform(mc, n, 16, dummies)
                    np.column_stack((z.real, z.imag)).astype('<i2').tofile(path)
                    cmd = [sys.executable, str(ROOT / 'host/onair.py'), str(path), '--rate', '40000000',
                           '--symbol-rate', str(40e6/n), '--scale', '512', '--modcod', str(mc),
                           '--stream', '16', '--receiver', str(receiver)]
                    result = subprocess.run(cmd, text=True, capture_output=True)
                    r = json.loads(result.stdout)
                    assert result.returncode == 0, (cmd, r, result.stderr)
                    assert r['payload_match'] and r['payload_frames'] >= 10, r
                    assert r['bch_failed_frames'] == 0 and r['ldpc_checks_before'] == 0, r
                    r.update({'evidence': 'host loopback, no hardware', 'modcod': mc, 'sps': n,
                              'dummy_before': dummies, 'host_generated_frames': 16})
                    with output.open('a') as f:
                        f.write(json.dumps(r, sort_keys=True) + '\n')
                    print(json.dumps(r, sort_keys=True))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--loopback', action='store_true')
    p.add_argument('--receiver', type=pathlib.Path)
    p.add_argument('--out', type=pathlib.Path)
    args = p.parse_args()
    test_dummy()
    test_mer()
    if args.loopback:
        if args.receiver is None or args.out is None:
            p.error('--loopback needs --receiver and --out')
        loopback(args.receiver, args.out)
