"""Byte BCH rows and sparse 8PSK indices against the independent reference."""
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2 as d
import gen_dvbs2_fast as gen


def test_byte_bch():
    rng = np.random.default_rng(20261016)
    for rate in d.RATES[3:]:
        c = d.code(rate)
        if c.t == 8:
            continue
        table = gen.bch_byte(c.t)
        r = c.nbch - c.kbch
        rows = []
        for v, expected in enumerate(d.bch_byte_table(False, c.t)):
            row = table[16*v:16*v+16] + table[4096+16*v:4096+16*v+16]
            assert row[r//8:] == bytes(32-r//8)
            rows.append(int.from_bytes(row[:r//8], "big"))
            assert rows[-1] == expected
        # Full lengths include the two-byte tail in t=12 modes. Test zero,
        # ones and random data, then compare parity with the bit-serial model.
        for data in (bytes(c.kbch//8), bytes([255])*(c.kbch//8),
                     rng.bytes(c.kbch//8)):
            reg = 0
            for v in data:
                reg = ((reg << 8) & ((1 << r)-1)) ^ rows[(reg >> (r-8)) ^ v]
            bits = np.unpackbits(np.frombuffer(data, np.uint8))
            expected = np.packbits(d.bch_encode(bits, rate)[c.kbch:]).tobytes()
            assert reg.to_bytes(r//8, "big") == expected


def test_sparse_pairs():
    # The runner masks 0x333 after shifts 2 and 0. Every 12-bit index must
    # produce the same phases as the canonical three Gray-map planes.
    for value in range(4096):
        p0, p2, p1 = value & 15, (value >> 4) & 15, value >> 8
        expected = [(((p0 >> bit) & 1) ^ 1) |
                    (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2)
                    for bit in range(3, -1, -1)]
        seen = []
        for shift in (2, 0):
            index = (value >> shift) & 0x333
            for bit in (1, 0):
                seen.append((((index >> bit) & 1) ^ 1) |
                            (((index >> (8+bit)) & 1) << 1) |
                            (((index >> (4+bit)) & 1) << 2))
        assert seen == expected


if __name__ == "__main__":
    test_byte_bch()
    test_sparse_pairs()
    print("encoder tables: all byte remainders, full BCH tails and 4096 sparse map indices match")
