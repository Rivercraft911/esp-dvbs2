"""Host checks for host/onair.py: the payload generator and one loop of reference symbols.

Run: python3 tests/test_onair.py. No board or receiver is needed.
"""
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "host"))
import dvbs2  # noqa: E402
import onair  # noqa: E402


def test_payload():
    # xorshift32 from 2026, written out by hand for the first three steps.
    x, first = 2026, []
    for _ in range(3):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        first.append(x & 0xFF)
    data = onair.payload(4)
    assert list(data[:3]) == first
    assert len(data) == onair.LOOP_FRAMES * (dvbs2.code("1/2").kbch - 80) // 8 == 32128
    assert len(onair.payload(11)) == onair.LOOP_FRAMES * 7264
    for bad in (3, 12):
        try:
            onair.payload(bad)
        except ValueError:
            continue
        raise AssertionError(f"modcod {bad} accepted")
    print("onair payload: xorshift32 bytes, loop length per modcod, QPSK range")


def test_symbols():
    s = onair.symbols(4)
    assert len(s) == onair.LOOP_FRAMES * 33282
    assert np.allclose(abs(s), 1)
    phases = np.round(np.angle(s) / (np.pi / 4)).astype(int) % 8
    assert set(np.unique(phases[90:])) <= {1, 3, 5, 7}  # QPSK body and pilots, scrambled
    print("onair symbols: one loop of PL frames, unit circle, QPSK phases")


if __name__ == "__main__":
    test_payload()
    test_symbols()
