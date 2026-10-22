"""Natural-order MC6 oracle and the existing independently pinned TS fixtures.

This oracle uses the standard's bit addresses, not the decoder's transposed
parity layout, rotations, compressed messages, or four-lane schedule. The three
clean words must match the saved gr-dtv and LeanSDR digests before being used.
No recorded RF samples or measured noise performance are claimed by this file.
"""
import hashlib
import json
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2 as d
from dvbs2_tables import LDPC

N, K, M, Q = 64800, 43200, 21600, 60
ROWS = LDPC[("normal", "2/3")]


def natural_checks(variable):
    """Check indices touching one natural-order information or parity bit."""
    if not 0 <= variable < N:
        raise ValueError("MC6 variable outside 0..64799")
    if variable < K:
        group, bit = divmod(variable, 360)
        return tuple((address + Q * bit) % M for address in ROWS[group])
    parity = variable - K
    return (parity, parity + 1) if parity + 1 < M else (parity,)


def syndrome(bits):
    """Return H*bits over GF(2), with checks in natural parity order."""
    bits = np.asarray(bits)
    if bits.shape != (N,) or np.any((bits != 0) & (bits != 1)):
        raise ValueError("expected exactly 64800 binary bits")
    bits = bits.astype(np.uint8, copy=False)
    out = np.zeros(M, np.uint8)
    offsets = Q * np.arange(360)
    for group, addresses in enumerate(ROWS):
        value = bits[360 * group:360 * (group + 1)]
        for address in addresses:
            out[(address + offsets) % M] ^= value
    out ^= bits[K:]
    out[1:] ^= bits[K:-1]
    return out


def natural_graph():
    """Check-to-variable adjacency, independent of the decoder's row layout."""
    checks = [[] for _ in range(M)]
    for variable in range(N):
        for check in natural_checks(variable):
            checks[check].append(variable)
    return checks


def check_schedule():
    """Prove each selected tile touches disjoint variable sets across lanes."""
    checks = natural_graph()
    assert len(checks[0]) == 9 and all(len(row) == 10 for row in checks[1:])
    assert sum(map(len, checks)) == 215999
    tiles = 0
    for layer in range(Q):
        width = 4 if layer == 0 else 8
        for first in range(0, 360, width):
            used = [variable for column in range(first, first + width)
                    for variable in checks[layer + Q * column]]
            assert len(used) == len(set(used)), (layer, first)
            tiles += 1
    assert tiles == 2745
    # Eight lanes in layer 0 would change layered updates: checks 0 and 240
    # both touch information variable 0. This is a dependency, not padding.
    assert set(checks[0]).intersection(checks[Q * 4]) == {0}
    return tiles


def crosschecked_frames():
    """Return three (FEC bits, unscrambled BBFRAME bits) pairs.

    Recreate the seed-2026 TS input used for the pre-existing external digests.
    The hash gate is essential: a model-generated clean word alone would only
    check consistency between our encoder and decoder, not compatibility.
    """
    count = -(-3 * (43040 - 80) // 1504) + 2
    payload = bytearray()
    state = 2026
    while len(payload) < count * 187:
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        payload.extend(state.to_bytes(4, "little"))
    packets = np.empty((count, 188), np.uint8)
    packets[:, 0] = 0x47
    packets[:, 1:] = np.frombuffer(payload, np.uint8, count=count * 187).reshape(count, 187)
    bb = d.ts_bbframes(packets, "2/3")[:3]
    scrambled = [d.bb_scramble(frame) for frame in bb]
    bch = [d.bch_encode(frame, "2/3") for frame in scrambled]
    words = [d.ldpc_encode_groups(frame, "2/3") for frame in bch]
    recorded = json.loads((ROOT / "tests/data/crosscheck_digests.json").read_text())
    stages = {"bb": bb, "scr": scrambled, "bch": bch, "fec": words}
    for name, frames in stages.items():
        digest = hashlib.sha256(np.packbits(np.concatenate(frames)).tobytes()).hexdigest()[:32]
        for implementation in ("gr-dtv", "leansdr"):
            want = recorded[implementation]["normal 2/3 pilots"][name]
            if digest != want:
                raise AssertionError((implementation, name, digest, want))
    return list(zip(words, bb))


def self_check():
    # The standard's worked i0/i1 examples pin the first table row and q.
    first = (0, 10491, 16043, 506, 12826, 8065, 8226,
             2767, 240, 18673, 9279, 10579, 20928)
    assert natural_checks(0) == first
    assert natural_checks(1) == (60, 10551, 16103, 566, 12886, 8125, 8286,
                                 2827, 300, 18733, 9339, 10639, 20988)
    assert len(ROWS) == 120 and sum(map(len, ROWS)) == 480
    assert [len(row) for row in ROWS] == [13] * 12 + [3] * 108
    assert natural_checks(K) == (0, 1)
    assert natural_checks(N - 1) == (M - 1,)
    check_schedule()
    # Include group, word, transpose, parity-layer, and final-bit boundaries.
    basis = (0, 1, 29, 30, 31, 32, 59, 60, 359, 360, 4319, 4320,
             K - 1, K, K + 59, K + 60, N - 2, N - 1)
    for variable in basis:
        bits = np.zeros(N, np.uint8)
        bits[variable] = 1
        got = tuple(np.flatnonzero(syndrome(bits)).tolist())
        assert got == tuple(sorted(natural_checks(variable))), (variable, got)
    frames = crosschecked_frames()
    for bits, _ in frames:
        assert not syndrome(bits).any()
        for variable in basis:
            flipped = bits.copy()
            flipped[variable] ^= 1
            assert tuple(np.flatnonzero(syndrome(flipped))) == tuple(sorted(natural_checks(variable)))
    assert all(not np.array_equal(frames[0][0], item[0]) for item in frames[1:])
    print("MC6 oracle: three changing words match external stage digests; natural syndrome and boundary columns pass")


if __name__ == "__main__":
    self_check()
