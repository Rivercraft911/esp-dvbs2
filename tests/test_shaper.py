"""Checks for host/shaper.py and the firmware's table build and portable kernel.

Run: python3 tests/test_shaper.py. The firmware check compiles firmware/main/shaper.c with the
host C compiler (skipped if there is none). An optional argument checks records from
tests/shaper_pie.c running in QEMU; board checks use membench and host/shaper.py verify.
"""
import pathlib
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import replace

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import shaper as s  # noqa: E402


def test_qpsk_phase():
    b_i, b_q = np.array([0, 1, 1, 0]), np.array([0, 0, 1, 1])
    k = s.qpsk_phase(b_i, b_q)
    point = ((1 - 2 * b_i) + 1j * (1 - 2 * b_q)) / np.sqrt(2)          # dvbs2.py convention
    assert np.allclose(np.exp(1j * np.pi * k / 4), point) and k.tolist() == [1, 3, 5, 7]
    print("qpsk_phase: (bI, bQ) = 00 10 11 01 -> k = 1 3 5 7, the points of dvbs2.py")


def test_tables():
    for p in s.PROFILES.values():
        t = p.table
        assert t.shape == (p.windows, p.block_words) and not (t >> 20).any()
        assert not t[:, p.parts_count * p.row_words:].any()
        for j in range(p.parts_count):
            assert not t[:, j*p.row_words+p.n:(j+1)*p.row_words].any()
        for r, b in zip(p.parts, p.biases):
            assert (b.sum(axis=0) == 512).all()
            assert (r.min(axis=1) + b >= 0).all() and ((r.max(axis=1) + b).sum(axis=0) <= 1023).all()
    print("tables: every part field in 0..1023, part biases sum to 512, worst-case sum of three "
          "parts <= 1023 (no carry from I into Q), bits 31:20 and padding zero")


def direct(p, sym):
    """Output symbol by symbol from the taps, without the table: per part, sum the three
    symbols' level * tap exactly, round, add the parts."""
    k = np.asarray(sym, np.int64) >> 4
    out = []
    for n in range(s.HISTORY, len(k)):
        for q in range(p.n):
            v = []
            for level in (s.LEVEL_I, s.LEVEL_Q):
                total = 0
                for j in range(s.PARTS):
                    acc = sum(int(level[k[n - a]]) * int(p.taps[a * p.n + q])
                              for a in range(3 * j, 3 * j + 3))
                    total += (acc + (1 << (s.SHIFT - 1))) >> s.SHIFT
                v.append(total)
            out.append(complex(*v))
    return np.array(out)


def test_model():
    for p in s.PROFILES.values():
        sym = s.random_symbols(p, 60, 11)
        y = s.iq(s.shape(p, sym))
        assert np.array_equal(y, direct(p, sym)), p.name
        err = y - s.linear(p, sym)
        assert np.abs(np.r_[err.real, err.imag]).max() <= 1.5, p.name
    print(f"model: table output equals a per-part sum computed from the taps ({len(s.PROFILES)} profiles, 52 "
          "symbols each); within 1.5 LSB of the exact filter (three roundings of 0.5)")


def test_mask():
    for p in s.PROFILES.values():
        m = s.mask_margins(p.taps, p.n, p.alpha)
        assert min(m["upper"], m["lower"], m["far"]) > 0, (p.name, m)
        # Independent response: zero-padded FFT of the taps, hold applied, on its own grid.
        nfft = 1 << 20
        g = np.abs(np.fft.rfft(p.taps.astype(float), nfft))
        fn = np.arange(len(g)) * 2 * p.n / nfft                         # f / fN
        db = 20 * np.log10(g * np.abs(np.sinc(fn / (2 * p.n))) / g[0] + 1e-300)
        assert np.allclose(db[:4000], s.response_db(p.taps, p.n, fn[:4000]), atol=1e-6)
        up = np.array(s.MASK[p.alpha]["upper"])
        sel = fn <= p.n
        limit = np.interp(fn[sel], up[:, 0], up[:, 1], right=-40.0)
        assert (db[sel] < limit).all(), p.name
    print("mask: every profile inside the Annex A template with the ideal DAC hold (upper limit "
          "to Fs/2, lower to point M); the response agrees with an FFT of the taps")


def test_mer():
    for p in s.PROFILES.values():
        sym = s.cyclic(p, 2048, 3)
        m = s.mer(p, s.iq(s.shape(p, sym)), sym[s.HISTORY:])
        assert m > 35, (p.name, m)
    print("mer: table output through an ideal RRC receiver, with the hold, above 35 dB for all "
          "profiles (values: python3 host/shaper.py report)")


def test_hold_correction():
    # A finite inverse-transform pulse tends to the uncorrected RRC as Fs/Rs grows.
    assert np.max(np.abs(s.corrected_rrc(.2, 400) - s.window_rrc(.2, 400))) < 2e-6
    for p in s.PROFILES.values():
        old = replace(p, equalized=False)
        assert s.pulse_mer(p) > s.pulse_mer(old), p.name
        # Independent long FFT implements the same inverse hold, without quadrature.
        nfft = 1 << 18
        f = np.fft.fftfreq(nfft) * p.n
        h = s.rrc_response(f, p.alpha) / np.sinc(f / p.n)
        h = h * np.exp(-2j*np.pi*f/p.n*(len(p.taps)-1)/2)
        g = np.fft.ifft(h).real[:len(p.taps)] * p.n * np.kaiser(len(p.taps), s.KAISER_BETA)
        assert np.max(np.abs(g - s.corrected_rrc(p.alpha, p.n))) < 2e-7, p.name
    for span in (11, 13):
        p = replace(s.PROFILES['8psk-n4-a20'], span=span)
        sym = s.cyclic(p, 2048, 3)
        y = s.iq(s.shape(p, sym))
        assert len(y) == 2048*p.n
        assert np.max(np.abs(y - s.linear(p, sym))) < p.parts_count / np.sqrt(2)
        assert s.mer(p, y, sym[p.history:]) > 39
    print('hold correction: FFT agrees with quadrature, high-sps limit, pulse MER improves; longer spans checked')


def test_header():
    assert s.HEADER.read_text() == s.header_text(), "run: python3 host/shaper.py header"
    print("header: firmware/main/shaper_taps.h matches the model")


def test_firmware_c():
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        print("firmware_c: skipped, no host C compiler")
        return
    with tempfile.TemporaryDirectory() as tmp:
        (pathlib.Path(tmp) / "esp_attr.h").write_text("#define IRAM_ATTR\n")
        exe = pathlib.Path(tmp) / "shaper_host"
        main = ROOT / "firmware" / "main"
        subprocess.run([cc, "-O2", "-std=c11", "-Wall", "-Werror", f"-I{tmp}", f"-I{main}",
                        str(main / "shaper.c"), str(ROOT / "tests" / "shaper_host.c"),
                        "-o", str(exe)], check=True)
        out = subprocess.run([str(exe), tmp], check=True, capture_output=True, text=True).stdout
        for test, (name, count) in s.TESTS.items():
            p = s.PROFILES[name]
            got_words = np.fromfile(pathlib.Path(tmp) / f"{test}.bin", dtype=np.uint32)
            want_words = np.r_[p.table.ravel(), s.shape(p, s.random_symbols(p, count + s.HISTORY, s.SEED))]
            assert np.array_equal(got_words, want_words), test
    got = {k: int(v) for k, v in (line.split() for line in out.splitlines())}
    want = s.expected_checks()
    assert got == {k: v for k, v in want.items() if k.endswith(("_c", "_build"))}, (got, want)
    print("firmware_c: shaper.c compiled for the host builds the same tables and fills the same "
          "words as the model (checks " + ", ".join(f"{k} {v}" for k, v in got.items()) + ")")


def check_qemu(path):
    records = [json.loads(line[1:]) for line in pathlib.Path(path).read_text().splitlines()
               if line.startswith('@{"test":"shape')]
    want_cases = {(test, name, count, seed)
                  for test, names, counts in (
                      ('shape4_pie', ('8psk-n4-a20', '8psk-n4-a35'), (0,1,2,3,4,5,6,7,13,4093,4094,4095)),
                      ('shape4_rows', ('8psk-n4-a20', '8psk-n4-a35'), (0,1,2,3,4,5,6,7,13,4093,4094,4095)),
                      ('shape4_all', ('8psk-n4-a20', '8psk-n4-a35'), (0,1,2,3,4,5,6,7,13,4093,4094,4095)),
                      ('shape4_rows_all', ('8psk-n4-a20', '8psk-n4-a35'), (0,1,2,3,4,5,6,7,13,4093,4094,4095)),
                      ('shape5_pie', ('8psk-n5-a20', '8psk-n5-a35'), (0,4,8,12,16,3276)))
                  for name in names for count in counts for seed in (2026,7,0xffffffff)}
    assert len(records) == len(want_cases), len(records)
    for r in records:
        want_cases.remove((r['test'], r["profile"], r["symbols"], r["seed"]))
        p = s.PROFILES[r["profile"]]
        sym = s.random_symbols(p, s.HISTORY + r["symbols"], r["seed"])
        assert r["mismatch"] == 0 and r["guards"] == 1, r
        assert r["table_check"] == s.checksum(p.table.ravel()), r
        assert r["check"] == s.checksum(s.shape(p, sym)), r
    assert not want_cases
    print(f"qemu: {len(records)} PIE/C word comparisons and table/output checks equal the model")


if __name__ == "__main__":
    for fn in [v for k, v in dict(globals()).items() if k.startswith("test_")]:
        fn()
    if len(sys.argv) > 1:
        check_qemu(sys.argv[1])
