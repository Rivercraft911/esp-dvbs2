"""Offline capture checks. Run python3 tests/test_capture.py; numpy only, no hardware.

Synthetic RF is a windowed-sinc interpolation of BPSK samples, arbitrary start,
independent receive clock, carrier offset, unknown phase, Gaussian noise and
optional I/Q conjugation. This qualifies the algorithm on the stated model,
not any real receiver's analog bandwidth or sample timing.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import capture as c


class ArrayCapture:
    def __init__(self, z):
        self.z = z

    def __len__(self):
        return len(self.z)

    def read(self, first, last):
        return self.z[first:last].copy()


def synthetic(bits, rate=61_440_000, ppm=37.23, conjugate=False):
    count = 262144
    clock = c.DAC_RATE * (1 + ppm * 1e-6)
    t = np.arange(count) * clock / rate + 32.33
    index = np.floor(t).astype(int)
    symbols = 2 * bits.astype(float) - 1
    real = np.zeros(count)
    # Independent continuous-time interpolation, 25 taps with a Lanczos window.
    # At each integer source time the interpolation equals the original symbol.
    for k in range(-12, 13):
        distance = t - (index + k)
        real += symbols[index + k] * np.sinc(distance) * np.sinc(distance / 13)
    z = .25 * real * np.exp(1j * (.71 + 2 * np.pi * 12345 * np.arange(count) / rate))
    rng = np.random.default_rng(20261016)
    z += .003 * (rng.normal(size=count) + 1j * rng.normal(size=count))
    return ArrayCapture(z.conj() if conjugate else z)


def expect_error(fn):
    try:
        fn()
    except ValueError:
        return
    raise AssertionError("expected a rejected capture")


def test_counter():
    bits = c.tag_bits(0, 1000)
    state = 1
    for bit in bits:
        assert bit == state & 1
        state = c.tag_step(state)
    index = c.TagIndex()
    for first in [0, 1, 31, 14268, 1234567890, c.TAG_PERIOD - 17]:
        assert index.index(c.tag_jump(1, first)) == first
        word = sum(int(b) << i for i, b in enumerate(c.tag_bits(first, 32)))
        assert c._state(word) == c.tag_jump(1, first)
    expect_error(lambda: index.index(0))
    print("capture counter: bit convention, 32-bit state inversion, absolute seek to period edge")


def test_bit_faults():
    bits = c.tag_bits(654321, 100000)
    result = c.decode_bits(bits)
    assert result["first_source_sample"] == 654321
    assert result["continuity_pass"] and not result["recurrence_errors"]
    for size in [1, 14268, 16380]:
        gap = np.r_[bits[:20000], bits[20000 + size:]]
        repeat = np.r_[bits[:20000], bits[20000 - size:20000], bits[20000:]]
        for signal, field in [(gap, "gaps"), (repeat, "repeats")]:
            result = c.decode_bits(signal)
            assert not result["continuity_pass"]
            assert result[field] == size and len(result["events"]) == 1
            assert result["recurrence_errors"] > 0
    # A corrupt bit is not silently called a clean zero-gap capture.
    corrupt = bits.copy()
    corrupt[20000] ^= 1
    result = c.decode_bits(corrupt)
    assert not result["continuity_pass"] and result["unexplained_error_regions"] == 1
    assert result["gaps"] == result["repeats"] == 0
    rng = np.random.default_rng(9)
    expect_error(lambda: c.decode_bits(rng.integers(0, 2, 10000, dtype=np.uint8)))
    expect_error(lambda: c.decode_bits(np.zeros(10000, np.uint8)))
    # Incremental verification must check all 32-bit recurrences and retain
    # absolute counters even when a fault straddles an input-block boundary.
    for seam in [255, 256, 257, 262143, 262144, 262145]:
        long_bits = c.tag_bits(654321, max(300000, seam + 1000))
        changed = np.r_[long_bits[:seam], long_bits[seam + 1:]]
        expected = c.decode_bits(changed)
        verifier = c.TagVerifier()
        for first in range(0, len(changed), 257):
            verifier.feed(changed[first:first + 257])
        assert verifier.result() == expected, seam
    print("capture sequence: one sample, full step and full bank deletions/repeats; corrupt/random rejection; streaming seams")


def test_rf_tag():
    bits = c.tag_bits(456789, 400000)
    for ppm, rate, conjugate in [(37.23, 61_440_000, False),
                                 (-113.7, 50_000_000, True),
                                 (211.6, 61_440_000, True)]:
        result = c.analyze_tag(synthetic(bits, rate, ppm, conjugate), rate)
        assert result["continuity_pass"], result
        assert abs(result["dac_rate_relative_to_40mhz_ppm"] - ppm) < .2, result
        assert abs(abs(result["carrier_offset_hz"]) - 12345) < 1, result
        assert 456789 < result["first_source_sample"] < 456850
    for size, field in [(1, "gaps"), (14268, "gaps"), (1, "repeats"), (16380, "repeats")]:
        changed = (np.r_[bits[:20000], bits[20000 + size:]] if field == "gaps"
                   else np.r_[bits[:20000], bits[20000 - size:20000], bits[20000:]])
        result = c.analyze_tag(synthetic(changed), 61_440_000)
        assert result[field] == size and not result["continuity_pass"], result
    expect_error(lambda: c.analyze_tag(synthetic(bits), 40_000_000))
    # An SDR rate argument alone is insufficient: a 20 MHz total bandwidth
    # capture at 61.44 MS/s must fail recurrence/eye acquisition, not report zero.
    narrow = synthetic(bits)
    z = np.fft.fft(narrow.z)
    z[abs(np.fft.fftfreq(len(z), 1 / 61_440_000)) > 10_000_000] = 0
    narrow.z = np.fft.ifft(z)
    try:
        result = c.analyze_tag(narrow, 61_440_000)
        assert not result["continuity_pass"]
    except ValueError:
        pass
    print("capture RF tag: clock drift, CFO, unknown phase, noise, conjugation, sample/bank faults, narrow-band rejection")


def test_spectrum():
    rate, count = 16_000_000, 300000
    t = np.arange(count) / rate
    # Amplitude .25 => -12.0412 dBFS; image amplitude .0025 => -40 dBc.
    z = (.25 * np.exp(2j * np.pi * 1000037.25 * t)
         + .0025 * np.exp(-2j * np.pi * 1000037.25 * t)
         + .001 * np.exp(2j * np.pi * 5100111 * t) + .0005)
    result = c.spectrum(ArrayCapture(z), rate, "tone")
    assert abs(result["dominant_offset_hz"] - 1000037.25) < 2
    expect_error(lambda: c.spectrum(ArrayCapture(np.zeros(count, complex)), rate, "tone"))
    negative = ArrayCapture(.25 * np.exp(-2j * np.pi * 1000037.25 * t))
    try:
        c.spectrum(negative, rate, "tone")
    except ValueError as error:
        assert "dominant component is -" in str(error)
    else:
        raise AssertionError("negative-only tone was silently measured as +1 MHz")
    tone = result["tones"][0]
    assert abs(tone["offset_hz"] - 1000037.25) < 2
    assert abs(tone["power_dbfs"] - 20 * np.log10(.25)) < .02
    assert abs(tone["image_dbc"] + 40) < .02
    assert abs(result["strongest_spur_dbc"] + 40) < .02
    assert abs(result["dc_dbc"] - 20 * np.log10(.0005 / .25)) < .02
    # A 55 kHz carrier offset: image and LO leak sit around the carrier, not the SDR center.
    cfo = 55000.0
    shifted = (.25 * np.exp(2j * np.pi * (cfo + 1e6) * t) + .0025 * np.exp(2j * np.pi * (cfo - 1e6) * t)
               + .00025 * np.exp(2j * np.pi * cfo * t) + .0005)
    moved = c.spectrum(ArrayCapture(shifted), rate, "tone")
    assert abs(moved["carrier_offset_hz"] - cfo) < 2
    assert abs(moved["tones"][0]["image_dbc"] + 40) < .05
    assert abs(moved["lo_dbc"] + 60) < .05
    assert abs(moved["dc_dbc"] - 20 * np.log10(.0005 / .25)) < .05
    # 10 % AM at 610 Hz and 5 degrees of 13 kHz PM on that tone.
    tt = np.arange(1 << 20) / rate
    wobbly = ((1 + .1 * np.cos(2 * np.pi * 610 * tt)) * .25
              * np.exp(1j * (2 * np.pi * (cfo + 1e6) * tt + np.radians(5) * np.sqrt(2) * np.sin(2 * np.pi * 13000 * tt))))
    m = c.modulation(ArrayCapture(wobbly), rate, cfo + 1e6)
    assert abs(m["am_rms_pct"] - 10 / np.sqrt(2)) < .1, m
    assert abs(m["pm_rms_deg"] - 5) < .1, m
    assert abs(m["am_lines_hz"][0] - 610) < 20, m
    found = c.lines(ArrayCapture(shifted), rate, block=.005)
    assert abs(found["carrier_offset_hz"] - cfo) < 100, found
    assert abs(found["+1MHz"] - .25) < .002 and abs(found["-1MHz"] - .0025) < .0005, found
    assert abs(found["+0MHz"] - .00025) < .0002, found
    z += .25 * np.exp(2j * np.pi * 3000000 * t)
    result = c.spectrum(ArrayCapture(z), rate, "two")
    assert len(result["tones"]) == 2
    assert abs(result["tones"][1]["offset_hz"] - 3000000) < 2
    print("capture spectrum: fractional-bin tone offset, dBFS, image and LO around the carrier, AM/PM, line amplitudes, DC, strongest spur and two tones")


def test_files():
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "capture.iq"
        expected = np.tile([.25, -.125], (512, 1))
        for fmt, dtype, scale in [("i16", "<i2", 32768), ("f32", "<f4", 1)]:
            (expected * scale).astype(dtype).tofile(path)
            cap = c.Capture(path, fmt)
            assert len(cap) == 512
            assert np.allclose(cap.read(0, 512), .25 - .125j)
        (expected * 2048).astype("<i2").tofile(path)
        assert np.allclose(c.Capture(path, "i16", 2048).read(0, 512), .25 - .125j)
        expect_error(lambda: c.Capture(path, "i16", 0))
        path.write_bytes(b"abc")
        expect_error(lambda: c.Capture(path, "i16"))
        (expected * np.nan).astype("<f4").tofile(path)
        expect_error(lambda: c.Capture(path, "f32").read(0, 512))
        # CLI must fail closed with a JSON error and nonzero exit status.
        process = subprocess.run([sys.executable, str(ROOT / "host/capture.py"), str(path),
                                  "--format", "f32", "--rate", "40000000", "--signal", "tag"],
                                 text=True, capture_output=True)
        assert process.returncode == 2 and "error" in json.loads(process.stdout)
    print("capture files: little-endian int16/float32 scaling, truncation/nonfinite errors, CLI failure status")


if __name__ == "__main__":
    test_counter()
    test_bit_faults()
    test_rf_tag()
    test_spectrum()
    test_files()
