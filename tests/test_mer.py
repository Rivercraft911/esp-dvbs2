"""Synthetic qualification of data-aided MER. No hardware, receiver or saved dumps.

Unit-energy RRC symbols have sample power ~1. Independent complex AWGN has
sample variance Fs/Rs * 10**(-EsN0/10), so a unit-energy matched filter yields
symbol error/signal power 10**(-EsN0/10). This qualifies the stated signal
model, not an analog receive path. All frequency estimates inherit nominal Fs.
"""
import json
import pathlib
import sys
import tempfile

import numpy as np
from scipy.special import j0
from scipy.signal import upfirdn

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import iq
import onair


def synthetic(esn0=None, cfo=0, ppm=0, phase_rms=0, rate=4000000, first=87351):
    loop = onair.symbols(4)
    s = loop[(first + np.arange(48000)) % len(loop)]
    phases = (np.rint(np.angle(s) * 4 / np.pi).astype(int) % 8).astype(np.uint8)
    snr = None if esn0 is None else esn0 - 10 * np.log10(rate / 1e6 * (1 + ppm * 1e-6))
    z = iq.shape(phases, 1e6, rate, span=32, snr_db=snr, frequency_offset=cfo,
                 sample_rate_offset_ppm=ppm, seed=20261016)
    # Sinusoidal phase noise, 2 kHz and stated radians rms, no phase tracker in MER.
    t = np.arange(len(z)) / (rate * (1 + ppm * 1e-6))
    z = z * np.exp(1j * (.713 + phase_rms * np.sqrt(2) * np.sin(2 * np.pi * 2000 * t)))
    return z[123:-80]


def rejected(fn):
    try:
        fn()
    except ValueError:
        return
    raise AssertionError("expected rejected acquisition")


def test_impairments():
    for esn0, cfo, ppm in [(10, 58000, 0), (20, 55234, 73), (30, -50234, -110),
                           (20, -58000, 250), (30, 0, 0)]:
        z = synthetic(esn0, cfo, ppm)
        result = onair.recover_mer(z, 4000000)
        # +/-0.3 dB includes finite-noise variance and filter/interpolation error.
        assert abs(result["mer_db"] - esn0) < .3, result
        expected_cfo = cfo / (1 + ppm * 1e-6)
        assert abs(result["carrier_offset_hz"] - expected_cfo) < 2, result
        assert abs(result["sample_clock_offset_ppm"] - ppm) < 2, result
        assert result["symbols"] > 47000 and not result["phase_noise_tracked"]
        print("synthetic:", json.dumps({"esn0_db": esn0, "cfo_hz": cfo, "clock_ppm": ppm,
                                          "mer_db": result["mer_db"]}))
    # For sinusoidal phase, h = E[exp(j*phi)] = J0(sqrt(2)*sigma).
    # MER = |h|^2 / (1-|h|^2 + N0/Es), because only a constant/linear phase is fitted.
    sigma = .08
    h2 = j0(np.sqrt(2) * sigma)**2
    expected = 10 * np.log10(h2 / (1 - h2 + 10**(-30 / 10)))
    z = synthetic(30, 55234, 87, sigma)
    result = onair.recover_mer(z, 4000000)
    assert abs(result["mer_db"] - expected) < .3, (result, expected)
    assert result["mer_db"] < 23
    print(f"synthetic phase noise: rms {sigma} rad, predicted {expected:.3f} dB, recovered {result['mer_db']:.3f} dB")
    # A clean 40 MS/s model should not be limited by the acquisition grid.
    result = onair.recover_mer(synthetic(rate=40000000), 40000000)
    assert result["mer_db"] > 50, result
    print(f"synthetic clean 40 MS/s: {result['mer_db']:.3f} dB MER")


def test_rejections_and_files():
    rng = np.random.default_rng(8)
    rejected(lambda: onair.recover_mer(np.zeros(40000, complex), 4000000))
    rejected(lambda: onair.recover_mer(rng.normal(size=40000) + 1j * rng.normal(size=40000), 4000000))
    rejected(lambda: onair.recover_mer(synthetic()[:16000], 4000000))
    rejected(lambda: onair.recover_mer(synthetic(), 1000000))
    rejected(lambda: onair.recover_mer(synthetic(), 4000000, shift=float("nan")))
    unrelated = iq.shape(rng.integers(0, 4, 40000, dtype=np.uint8) * 2 + 1, 1e6, 4e6)
    rejected(lambda: onair.recover_mer(unrelated, 4000000))
    z = synthetic(20, 55000, 50)
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "test.iq"
        raw = np.column_stack([z.real, z.imag]) * 200
        np.rint(raw).astype("<i2").tofile(path)
        result = onair.capture_mer(path, 4000000)
        assert abs(result["mer_db"] - 20) < .3
        path.write_bytes(b"abc")
        rejected(lambda: onair.capture_mer(path, 4000000))
    print("MER: arbitrary payload/frame start, CFO, clock error, phase noise, file scaling; noise/unrelated/truncated captures rejected")


def test_dump_delay():
    # Independent causal pulse centered at sample 17.5. A filter evaluated at
    # n+frac observes the signal at delay+frac, so the reported sign must be +.
    taps = iq.rrc((np.arange(36) - 17.5) / 4, .35)
    z = upfirdn(taps, onair.symbols(4)[:4096], up=4)
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "dump.iq"
        np.rint(100 * np.column_stack([z.real, z.imag])).astype("<i2").tofile(path)
        result = onair.dump_mer(path, 4000000, 1000000, 512, 4)
        assert abs(result["delay_samples"] - 17.5) < .126, result
    print("legacy dump MER: fractional-delay reporting sign matches the causal pulse")


if __name__ == "__main__":
    test_impairments()
    test_rejections_and_files()
    test_dump_delay()
