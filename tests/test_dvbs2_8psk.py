"""Independent formula checks for QPSK/8PSK phase output, EN 302 307-1 V1.4.1.

Source: https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/
en_30230701v010401p.pdf, pp. 26-28 and 31-35. No external transmitter code is used.
Run: python3 tests/test_dvbs2_8psk.py (numpy, no hardware).
"""
from functools import lru_cache
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "host"))
import dvbs2 as d  # noqa: E402

RATES = ("3/5", "2/3", "3/4", "5/6", "8/9", "9/10")  # Table 12, MODCOD 12..17
MODES = [(s, r) for s in (False, True) for r in RATES if not (s and r == "9/10")]
rng = np.random.default_rng(20261016)


def interleaver_indices(n, rate):
    """5.3.3: out[3j+k] = in[j + rows*k], replacing k by 2-k for rate 3/5."""
    rows = n // 3
    return np.array([j + rows * (2 - k if rate == "3/5" else k)
                     for j in range(rows) for k in range(3)])


def test_interleaver():
    for short, rate in MODES:
        n = 16200 if short else 64800
        # Distinct integer labels check every address, including column and frame boundaries.
        labels = np.arange(n)
        want = interleaver_indices(n, rate)
        got = d.bit_interleave(labels, rate)
        assert np.array_equal(got, want)
        assert np.array_equal(np.sort(got), labels)
        inverse = np.empty(n, np.int64)
        inverse[got] = labels
        assert np.array_equal(got[inverse], labels)
        assert np.array_equal(d.bit_interleave(labels, rate, "qpsk"), labels)
        first = [2 * n // 3, n // 3, 0] if rate == "3/5" else [0, n // 3, 2 * n // 3]
        assert got[:3].tolist() == first
    print("interleaver: every address, 11 normal/short modes; rate 3/5 reads columns 3,2,1, "
          "others 1,2,3 (5.3.3, Figures 7/8, Table 8)")


# Figure 10 labels read counter-clockwise from the positive I axis. This representation is
# deliberately the inverse of the implementation's binary-label-to-phase table.
LABELS = ("001", "000", "100", "110", "010", "011", "111", "101")
CONSTELLATION = {label: np.exp(1j * np.pi * k / 4) for k, label in enumerate(LABELS)}


def test_mapping():
    triples = np.array([[int(b) for b in label] for label in LABELS], np.uint8)
    phases = d.psk8_map(triples.ravel())
    assert phases.dtype == np.uint8 and phases.tolist() == list(range(8))
    assert np.allclose(d.symbols_from_phases(phases), list(CONSTELLATION.values()))
    assert np.allclose(np.abs(d.symbols_from_phases(phases)) ** 2, 1)
    for a, b in zip(LABELS, LABELS[1:] + LABELS[:1]):
        assert sum(x != y for x, y in zip(a, b)) == 1  # Gray labels, including wraparound
    print("mapping: all eight Figure 10 labels, absolute phase, unit energy, Gray neighbours")


def header_complex(modcod, short, pilots):
    """5.5.2, Figure 13b: G rows are the five binary digits of column i, then all ones."""
    y = np.full(32, int(short), np.uint8)
    for k in range(5):
        y ^= ((modcod >> (4 - k)) & 1) * ((np.arange(32) >> k) & 1).astype(np.uint8)
    bits = np.repeat(y, 2)
    bits[1::2] ^= int(pilots)
    scramble = np.array([int(b) for b in f"{0x719D83C953422DFA:064b}"], np.uint8)
    bits ^= scramble
    sof = np.array([int(b) for b in "01100011010010111010000010"], np.uint8)
    signs = 1.0 - 2.0 * np.concatenate([sof, bits])
    # Odd/even here are the standard's 1-based symbol indices (5.5.2).
    return signs * np.where(np.arange(90) % 2, -1 + 1j, 1 + 1j) / np.sqrt(2)


@lru_cache(maxsize=1)
def gold_sequences():
    """5.5.4: x(i+18)=x(i+7)+x(i), y(i+18)=y(i+10)+y(i+7)+y(i+5)+y(i), modulo 2."""
    length = 2 ** 18 - 1
    x, y = [1] + [0] * (length - 1), [1] * length
    for i in range(length - 18):
        x[i + 18] = (x[i + 7] + x[i]) % 2
        y[i + 18] = (y[i + 10] + y[i + 7] + y[i + 5] + y[i]) % 2
    return np.array(x, np.uint8), np.array(y, np.uint8)


def rotations(length, n):
    x, y = gold_sequences()
    period = 2 ** 18 - 1
    i = np.arange(length)
    z0 = (x[(i + n) % period] + y[i]) % 2
    late = (i + 131072) % period
    z1 = (x[(late + n) % period] + y[late]) % 2
    return np.exp(1j * np.pi * (2 * z1 + z0) / 2)


def frame_complex(fec, rate, short, pilots, n):
    """5.3.3, Figure 10 and 5.5 formulas, using complex multiplication rather than phases."""
    bits = fec[interleaver_indices(len(fec), rate)].reshape(-1, 3)
    symbols = np.array([CONSTELLATION["".join(map(str, row))] for row in bits])
    slots = len(symbols) // 90
    blocks = (slots - 1) // 16 if pilots else 0
    body = np.full(len(symbols) + 36 * blocks, (1 + 1j) / np.sqrt(2))
    # Slot s starts after 36 floor(s/16) pilot symbols, except when pilots are disabled.
    for slot in range(slots):
        start = 90 * slot + (36 * (slot // 16) if pilots else 0)
        body[start:start + 90] = symbols[90 * slot:90 * (slot + 1)]
    body *= rotations(len(body), n)
    return np.concatenate([header_complex(12 + RATES.index(rate), short, pilots), body])


def test_frames():
    for short, rate in MODES:
        for pilots in (False, True):
            for n in (0, 262141):  # final valid scrambling-code range, including period wrap
                fec = rng.integers(0, 2, 16200 if short else 64800, dtype=np.uint8)
                p = d.plframe_phases(fec, rate, short, pilots, n, "8psk")
                want = frame_complex(fec, rate, short, pilots, n)
                slots = (60 if short else 240)
                expected_length = 90 * (slots + 1) + (36 * ((slots - 1) // 16) if pilots else 0)
                assert len(p) == len(want) == expected_length
                assert len(p) == d.plframe_length(short, pilots, "8psk")
                assert p.dtype == np.uint8 and p.max() < 8
                assert np.max(np.abs(d.symbols_from_phases(p) - want)) < 2e-15
    assert d.plframe_length(False, True, "8psk") == 22194
    assert d.plframe_length(False, False, "8psk") == 21690
    assert d.plframe_length(True, True, "8psk") == 5598
    assert d.plframe_length(True, False, "8psk") == 5490
    print("PL frames: 44 frames, MODCOD 12..17, independent header/Gold formulas, 90-symbol "
          "slots, pilot placement and phase rotations; normal pilots=22194, short=5598 symbols")


def test_pilot_boundaries():
    for short in (False, True):
        fec = np.zeros(16200 if short else 64800, np.uint8)
        phases = d.plframe_phases(fec, "2/3", short, True, 0, "8psk")[90:]
        descrambled = d.symbols_from_phases(phases) / rotations(len(phases), 0)
        slots = 60 if short else 240
        keep = np.ones(len(phases), bool)
        for block in range(1, (slots - 1) // 16 + 1):
            start = block * (16 * 90 + 36) - 36
            keep[start:start + 36] = False
            assert np.allclose(descrambled[start:start + 36], (1 + 1j) / np.sqrt(2))
        assert keep.sum() == len(fec) // 3
        assert keep[-1]  # no trailing pilot block, also when slot count is divisible by 16
        # With all-zero triples data and pilots both have pi/4 phase. Check a second frame
        # whose 001 triples have phase 0 so one displaced data/pilot symbol will be detected.
        source = np.zeros((3, len(fec) // 3), np.uint8)
        source[2] = 1
        phases = d.plframe_phases(source.ravel(), "2/3", short, True, 0, "8psk")[90:]
        descrambled = d.symbols_from_phases(phases) / rotations(len(phases), 0)
        assert np.allclose(descrambled[keep], 1)
        assert np.allclose(descrambled[~keep], (1 + 1j) / np.sqrt(2))
    print("pilots: exact 36-symbol boundaries after 16 slots; 14 normal / 3 short blocks, "
          "none after the final slot")


def test_qpsk_compatibility():
    for short, rate, pilots, n in ((False, "1/2", True, 0), (False, "9/10", False, 7),
                                   (True, "1/4", True, 262141)):
        fec = rng.integers(0, 2, d.code(rate, short).n, dtype=np.uint8)
        bits = d.plframe(fec, rate, short, pilots, n)
        phases = d.plframe_phases(fec, rate, short, pilots, n)
        assert np.allclose(d.symbols(*bits), d.symbols_from_phases(phases))
        assert len(phases) == d.plframe_length(short, pilots)
    assert [d.modcod(r, "8psk") for r in RATES] == list(range(12, 18))
    for invalid in ("1/2", "4/5"):
        try:
            d.modcod(invalid, "8psk")
        except ValueError:
            pass
        else:
            raise AssertionError(f"8PSK {invalid} has no MODCOD in Table 12")
    print("compatibility: QPSK phase bytes reproduce legacy sign bits; invalid 8PSK rates rejected")


def test_gcs_roundtrip():
    rate = "2/3"
    capacity = d.code(rate).kbch - 80
    data = rng.integers(0, 2, capacity + 13, dtype=np.uint8)
    phases = d.transmit_phases(data, rate, pilots=True, modulation="8psk")
    length, decoded = 22194, []
    for start in range(0, len(phases), length):
        body = d.symbols_from_phases(phases[start + 90:start + length])
        body /= rotations(len(body), 0)
        positions = np.arange(21600) + 36 * (np.arange(21600) // 1440)
        # Nearest ideal point recovers labels independently of the phase-index map.
        points = np.array(list(CONSTELLATION.values()))
        labels = np.array([[int(b) for b in s] for s in LABELS], np.uint8)
        triplets = labels[np.argmin(np.abs(body[positions, None] - points), axis=1)]
        fec = np.empty(64800, np.uint8)
        fec[interleaver_indices(64800, rate)] = triplets.ravel()
        bbf = d.bb_scramble(fec[:d.code(rate).kbch])
        assert d.crc8(bbf[:80]) == 0
        dfl = int("".join(map(str, bbf[32:48])), 2)
        decoded.append(bbf[80:80 + dfl])
        assert not bbf[80 + dfl:].any()
    assert [len(a) for a in decoded] == [capacity, 13]
    assert np.array_equal(np.concatenate(decoded), data)
    print("GCS: two 8PSK frames reversed through scrambling, pilots, mapping and interleaving; "
          "BBHEADER CRC, final 13-bit DFL and zero padding checked")


if __name__ == "__main__":
    for test in (test_interleaver, test_mapping, test_frames, test_pilot_boundaries,
                 test_qpsk_compatibility, test_gcs_roundtrip):
        test()
    print("8PSK checks passed")
