"""Check guarded products and BCH folding without opening the board."""
import ctypes
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "host"))
from dvbs2 import bch_generator


def clmul(a, b):
    product = 0
    while b:
        if b & 1:
            product ^= a
        a <<= 1
        b >>= 1
    return product


def divmod_poly(a, g):
    quotient = 0
    while a.bit_length() >= g.bit_length():
        bit = a.bit_length() - g.bit_length()
        quotient ^= 1 << bit
        a ^= g << bit
    return quotient, a


def limbs(value, count):
    return (ctypes.c_uint32 * count)(*[
        (value >> (32 * i)) & 0xffffffff for i in range(count)])


def integer(words):
    return sum(int(v) << (32 * i) for i, v in enumerate(words))


def products(lib, rng):
    rows = []
    for bits, spacing in [(64, 7), (248, 8)]:
        mask = (1 << bits) - 1
        cases = [(0, 0), (mask, mask), (1, mask),
                 (1 << (bits - 1), 1 << (bits - 1))]
        cases += [(rng.getrandbits(bits), rng.getrandbits(bits)) for _ in range(1000)]
        digest = hashlib.sha256()
        words = (bits * spacing + 31) // 32
        for a, b in cases:
            pa = (ctypes.c_uint32 * words)()
            pb = (ctypes.c_uint32 * words)()
            lib.poly_pack(pa, limbs(a, 8), bits, spacing)
            lib.poly_pack(pb, limbs(b, 8), bits, spacing)
            assert integer(pa) == sum(((a >> i) & 1) << (spacing * i) for i in range(bits))
            assert integer(pb) == sum(((b >> i) & 1) << (spacing * i) for i in range(bits))
            product = integer(pa) * integer(pb)
            got = (ctypes.c_uint32 * ((2 * bits - 1 + 31) // 32))()
            lib.poly_extract(got, limbs(product, 2 * words), 2 * bits - 1, spacing)
            oracle = (ctypes.c_uint32 * len(got))()
            lib.poly_reference(oracle, limbs(a, 8), limbs(b, 8), bits)
            assert integer(got) == integer(oracle) == clmul(a, b)
            digest.update(a.to_bytes(32, "little") + b.to_bytes(32, "little") +
                          integer(got).to_bytes(64, "little"))
        rows.append(dict(bits=bits, spacing=spacing, cases=len(cases),
                         hardware_words=words, coefficient_bound=bits,
                         lane_radix=1 << spacing, exact=True, digest=digest.hexdigest()))
    return rows


def folds(rng):
    rows = []
    for t in [8, 10, 12]:
        g = bch_generator(False, t)
        r = g.bit_length() - 1
        for k in [64, 248]:
            reciprocal, _ = divmod_poly(1 << (r + k), g)
            for _ in range(1004):
                previous = rng.getrandbits(r)
                data = rng.getrandbits(k)
                value = (previous << k) ^ (data << r)
                top = value >> r
                quotient = top ^ (clmul(top, reciprocal ^ (1 << k)) >> k)
                reduced = value ^ clmul(quotient, g)
                assert reduced == divmod_poly(value, g)[1] and reduced.bit_length() <= r
            rows.append(dict(t=t, parity_bits=r, chunk_bits=k, cases=1004,
                             exact=True, generator=hex(g), reciprocal=hex(reciprocal)))
    return rows


def main():
    rng = random.Random(20261019)
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / "poly.so"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(HERE / "main/poly.c"), "-o", str(library)], check=True)
        rows = products(ctypes.CDLL(str(library)), rng)
    report = dict(passed=True, scope="host arithmetic only, no hardware timing",
                  product_cases=sum(row["cases"] for row in rows), fold_cases=6024,
                  products=rows, folds=folds(rng),
                  script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  c_sha256=hashlib.sha256((HERE / "main/poly.c").read_bytes()).hexdigest())
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
