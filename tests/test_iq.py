"""RRC, impairment and capture-format checks. Run with python3 tests/test_iq.py."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import iq  # noqa: E402


def test_pulse():
    for alpha in (0.20, 0.35):
        special = np.array([0, 1 / (4 * alpha), -1 / (4 * alpha)])
        want = [1 + alpha * (4 / np.pi - 1)]
        want += [alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) +
                                      (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))] * 2
        assert np.allclose(iq.rrc(special, alpha), want, atol=1e-14)
        t = np.linspace(-80, 80, 128001)
        h = iq.rrc(t, alpha)
        assert np.max(np.abs(h - h[::-1])) < 1e-12
        assert abs(np.trapezoid(h * h, t) - 1) < 1e-6
        # Two RRC filters give the raised-cosine Nyquist pulse. Finite 12-symbol
        # tails leave at most 0.6% residual ISI at other symbol sampling times.
        p = iq.shape(np.array([0]), 1e6, 4e6, alpha)
        matched = np.convolve(p, p[::-1].conj())[::4] / 4
        center = len(matched) // 2
        assert abs(matched[center] - 1) < 0.0003
        assert np.max(np.abs(np.delete(matched, center))) < 0.006
        for phase in range(8):
            got = iq.shape(np.array([phase]), 1e6, 4e6, alpha)
            assert np.allclose(got, p * np.exp(1j * np.pi * phase / 4), atol=1e-7)
    print("RRC: removable poles, even symmetry, unit energy, matched-filter Nyquist samples")


def test_clock_and_carrier():
    phases = np.array([0, 1, 7, 3, 5], np.uint8)
    symbols = np.exp(1j * np.pi * phases / 4)
    # Check both rational polyphase and arbitrary-clock paths against a direct
    # continuous-time pulse sum, so resampling cannot hide sign or time errors.
    for rate, ppm in ((4e6, 0), (2.37e6, 0), (4e6, 50), (4e6, -50), (4e6, np.pi)):
        actual = rate * (1 + ppm * 1e-6)
        got = iq.shape(phases, 1e6, rate, sample_rate_offset_ppm=ppm)
        t = np.arange(len(got)) * 1e6 / actual - 6
        want = np.zeros(len(got), np.complex128)
        for k, symbol in enumerate(symbols):
            pulse = iq.rrc(t - k, 0.35)
            want += symbol * np.where(np.abs(t - k) <= 6, pulse, 0)
        assert np.max(np.abs(got - want)) < 2e-7
        offset = 12345.0
        shifted = iq.shape(phases, 1e6, rate, frequency_offset=offset,
                           sample_rate_offset_ppm=ppm)
        rotation = np.exp(2j * np.pi * offset * np.arange(len(got)) / actual)
        assert np.max(np.abs(shifted - got * rotation)) < 2e-7
    phases = np.arange(10000, dtype=np.int64) % 8
    plain = iq.shape(phases, 1e6, 4e6)
    fast = iq.shape(phases, 1e6, 4e6, sample_rate_offset_ppm=500)
    assert len(fast) > len(plain)  # a faster ADC takes more samples in the same time
    print("clock/carrier: direct pulse sums at integer, fractional and offset rates; CFO sign")


def test_noise():
    rng = np.random.default_rng(20261016)
    phases = rng.integers(0, 8, 50000, dtype=np.uint8)
    signal = iq.shape(phases, 1e6, 4e6)
    noisy = iq.shape(phases, 1e6, 4e6, snr_db=10, seed=19)
    assert np.array_equal(noisy, iq.shape(phases, 1e6, 4e6, snr_db=10, seed=19))
    assert not np.array_equal(noisy, iq.shape(phases, 1e6, 4e6, snr_db=10, seed=20))
    signal, noise = signal[24:-24], (noisy - signal)[24:-24]
    measured = 10 * np.log10(np.mean(np.abs(signal) ** 2) / np.mean(np.abs(noise) ** 2))
    assert abs(measured - 10) < 0.04
    assert abs(np.var(noise.real) / np.var(noise.imag) - 1) < 0.02
    assert abs(np.mean(noise)) < 0.002
    print(f"noise: seeded complex AWGN, measured sample SNR {measured:.3f} dB (asked 10 dB)")


def test_dac_hold():
    # A zero-order hold's in-band transfer function is derived by integrating
    # its rectangle from 0 to 1/Fdac. Test magnitude AND half-sample delay.
    raw = iq.shape(np.array([0]), 1, 64, span=128)
    held = iq.shape(np.array([0]), 1, 64, span=128, dac_rate=4)
    size = 32768
    spectrum = np.fft.fft(raw, size)
    ratio = np.fft.fft(held, size) / spectrum
    frequency = np.fft.fftfreq(size, 1 / 64)
    band = np.abs(frequency) < 0.5
    want = np.sinc(frequency / 4) * np.exp(-1j * np.pi * frequency / 4)
    assert np.max(np.abs(ratio[band] - want[band])) < 3e-5
    near_ideal = iq.shape(np.array([0]), 1, 4, dac_rate=1e10)
    ideal = iq.shape(np.array([0]), 1, 4)
    assert np.max(np.abs(near_ideal[1:-1] - ideal[1:-1])) < 1e-7
    print("DAC hold: sinc droop, causal half-DAC-period delay, infinite-rate limit")


def test_files():
    samples = np.array([1 + 2j, -3.5 + 0.25j], np.complex64)
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        raw = directory / "test.cfile"
        sigmf = directory / "test.sigmf-data"
        iq.write_capture(raw, samples, 4e6, 1e6)
        iq.write_capture(sigmf, samples, 4e6, 1e6, alpha=0.2, sample_rate_offset_ppm=50)
        assert raw.read_bytes() == sigmf.read_bytes()
        assert np.fromfile(raw, "<f4").tolist() == [1, 2, -3.5, 0.25]
        metadata = json.loads(sigmf.with_suffix(".sigmf-meta").read_text())
        assert metadata["global"]["core:datatype"] == "cf32_le"
        assert metadata["global"]["core:sample_rate"] == 4e6
        assert metadata["global"]["dvbs2:symbol_rate"] == 1e6
        assert metadata["global"]["dvbs2:rolloff"] == 0.2
        assert metadata["captures"] == [{"core:sample_start": 0}]
        settings = json.loads(metadata["global"]["core:description"])
        assert settings["sample_rate_offset_ppm"] == 50
        phase_path = directory / "phases.bin"
        phases = np.arange(8, dtype=np.uint8)
        phases.tofile(phase_path)
        run = subprocess.run([sys.executable, str(Path(iq.__file__)), str(phase_path),
                              str(sigmf), "--symbol-rate", "1000000", "--sample-rate", "4000000",
                              "--alpha", "0.2"], capture_output=True, text=True, check=True)
        assert json.loads(run.stdout)["samples"] == len(np.fromfile(sigmf, "<c8"))
        assert np.array_equal(np.fromfile(sigmf, "<c8"), iq.shape(phases, 1e6, 4e6, 0.2))
        symbols = np.array([0.2 + 0.3j, -1.2 + 0.7j, 0, 0.5 - 1j], dtype="<c8")
        symbols.tofile(phase_path)
        subprocess.run([sys.executable, str(Path(iq.__file__)), str(phase_path), str(sigmf),
                        "--complex-symbols", "--symbol-rate", "1000000",
                        "--sample-rate", "4000000"], capture_output=True, check=True)
        assert np.array_equal(np.fromfile(sigmf, "<c8"), iq.shape_symbols(symbols, 1e6, 4e6))
    print("files: cf32_le byte order, SigMF metadata and CLI/API agreement")


def test_complex_symbols():
    symbols = np.array([0.2 + 0.3j, -1.2 + 0.7j, 0, 0.5 - 1j])
    for ppm in (0, 50):
        got = iq.shape_symbols(symbols, 1e6, 4e6, sample_rate_offset_ppm=ppm)
        t = np.arange(len(got)) / (4 * (1 + ppm * 1e-6)) - 6
        want = sum(s * np.where(np.abs(t - k) <= 6, iq.rrc(t - k, 0.35), 0)
                   for k, s in enumerate(symbols))
        assert np.max(np.abs(got - want)) < 2e-7
        assert np.allclose(iq.shape_symbols(0.5 * symbols, 1e6, 4e6,
                                           sample_rate_offset_ppm=ppm), got * 0.5)
    for bad in ([], [[1j]], [np.nan + 1j], [np.inf]):
        try:
            iq.shape_symbols(bad, 1e6, 4e6)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid complex symbols accepted")
    print("complex symbols: direct pulse sums, amplitude scaling, integer/offset clocks, CLI")


def test_invalid():
    for phases, options in ((np.array([8]), {}), (np.array([-1]), {}),
                            (np.array([1.5]), {}), (np.array([], np.uint8), {}),
                            (np.array([0]), {"sample_rate": 1e6}),
                            (np.array([0]), {"dac_rate": 1e6}),
                            (np.array([0]), {"sample_rate_offset_ppm": -1e6}),
                            (np.array([0]), {"alpha": 0}),
                            (np.array([0]), {"span": 3})):
        settings = {"symbol_rate": 1e6, "sample_rate": 4e6, **options}
        try:
            iq.shape(phases, **settings)
        except ValueError:
            pass
        else:
            raise AssertionError((phases, settings))
    print("inputs: invalid phases, aliased rates and invalid filter/clock settings rejected")


if __name__ == "__main__":
    test_pulse()
    test_clock_and_carrier()
    test_noise()
    test_dac_hold()
    test_files()
    test_complex_symbols()
    test_invalid()
