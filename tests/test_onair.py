"""Host checks for host/onair.py: the payload stream, the reference symbols and the stream check.

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


def test_stream():
    # The 8-frame loop of firmware 35739b5 is the start of the continuous stream.
    per_frame = (dvbs2.code("1/2").kbch - 80) // 8
    stream = onair.payload(4, 20)
    assert stream[:8 * per_frame] == onair.payload(4)
    loop, longer = onair.symbols(4), onair.symbols(4, 9)
    assert np.array_equal(longer[:len(loop)], loop) and len(longer) == 9 * 33282

    match = onair.stream_match
    assert match(stream, stream[3 * per_frame:9 * per_frame], per_frame) == 3 * per_frame
    assert match(stream, stream, per_frame) == 0
    flipped = bytearray(stream[2 * per_frame:4 * per_frame])
    flipped[1234] ^= 0x10
    gap = stream[per_frame:3 * per_frame] + stream[4 * per_frame:6 * per_frame]
    loop_twice = stream[:8 * per_frame] * 2      # a repeating loop is not the stream
    for bad in (b"", stream[3 * per_frame + 1:9 * per_frame + 1],   # off a frame boundary
                stream[3 * per_frame:9 * per_frame - 1],             # partial frame
                bytes(flipped), gap, loop_twice):
        assert match(stream, bad, per_frame) is None
    print("onair stream: the 8-frame loop is the stream's start; a decoded payload matches only "
          "as whole frames from a frame boundary, and a flipped bit, a missing frame, a "
          "repeated loop or a shifted slice is rejected")


if __name__ == "__main__":
    test_payload()
    test_symbols()
    test_stream()
