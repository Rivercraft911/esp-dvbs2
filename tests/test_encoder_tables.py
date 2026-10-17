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


def test_packed_four():
    # Independent phase formula for every possible index, including the
    # removed P1 high bit. Inspect the actual generated words too.
    import re
    text = gen.generate().split("fast_map_3_packed[2048]", 1)[1].split("};", 1)[0]
    table = [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", text)]
    assert len(table) == 2048
    for v in range(4096):
        expected = 0
        for i in range(4):
            bit = 3 - i
            phase = (((v >> bit) & 1) ^ 1) | (((v >> (8 + bit)) & 1) << 1) | (((v >> (4 + bit)) & 1) << 2)
            expected |= phase << (4 * i)
        assert table[v & 2047] ^ ((v >> 10) & 2) == expected


def test_qpsk_packed():
    import re
    text = gen.generate().split("fast_map_2_packed[256]", 1)[1].split("};", 1)[0]
    table = [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", text)]
    for x in range(256):
        for g in range(256):
            dd = x ^ (x >> 1)
            index = ((dd ^ g) & 0x55) | (((x ^ (g >> 1) ^ (dd & g)) & 0x55) << 1)
            seen = [(table[index] >> (4*i)) & 15 for i in range(4)]
            expected = []
            for i in range(4):
                a, b = (x >> (7-2*i)) & 1, (x >> (6-2*i)) & 1
                phase = (1, 7, 3, 5)[2*a+b]
                rotation = ((g >> (7-2*i)) & 1)*2 + ((g >> (6-2*i)) & 1)
                expected.append((phase + 2*rotation) & 7)
            assert seen == expected


if __name__ == "__main__":
    test_byte_bch()
    test_sparse_pairs()
    test_packed_four()
    test_qpsk_packed()
    print("encoder tables: all byte remainders, full BCH tails and 4096 8PSK indices and all 65536 QPSK data/Gold byte pairs match")
