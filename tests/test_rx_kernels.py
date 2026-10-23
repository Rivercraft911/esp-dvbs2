"""Front-end kernel references (firmware/components/dvbs2rx/kernels.c) against numpy."""
import ctypes
import pathlib
import subprocess
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware/components/dvbs2rx"


class Step(ctypes.Structure):
    _fields_ = [("base", ctypes.c_uint32), ("coef", ctypes.c_void_p)]


def build(tmp):
    lib = pathlib.Path(tmp) / "kernels.so"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    "-o", str(lib), str(SRC / "kernels.c")], check=True)
    return ctypes.CDLL(str(lib))


def main():
    rng = np.random.default_rng(2026)
    with tempfile.TemporaryDirectory() as tmp:
        k = build(tmp)
        words = rng.integers(0, 2**32, 4096, dtype=np.uint32)
        i = np.zeros(4096 + 256, np.int8)
        q = np.zeros(4096 + 256, np.int8)
        k.rx_unpack(words.ctypes, ctypes.c_size_t(4096 // 16), i.ctypes, q.ctypes)
        s10i = ((words.astype(np.int64) >> 10) & 1023) - (((words >> 19) & 1) << 10)
        s10q = (words.astype(np.int64) & 1023) - (((words >> 9) & 1) << 10)
        assert np.array_equal(i[:4096], s10i // 4) and np.array_equal(q[:4096], s10q // 4)
        assert i.min() >= -128 and q.max() <= 127

        # Spans start at any sample s: word s of the bank, byte s of the rings.
        for s in range(17):
            for n in (0, 5, 16, 100, 1000):
                ri = np.zeros(1100, np.int8)
                rq = np.zeros(1100, np.int8)
                k.rx_unpack_span(words[s:].ctypes, ctypes.c_size_t(n),
                                 ctypes.c_void_p(ri.ctypes.data + s), ctypes.c_void_p(rq.ctypes.data + s))
                assert np.array_equal(ri[s:s + n], i[s:s + n]) and np.array_equal(rq[s:s + n], q[s:s + n])
                assert not ri[:s].any() and not ri[s + n:].any()

        for vectors in (2, 7, 9):
            sets = rng.integers(-128, 128, (8, 16 * vectors), dtype=np.int8)
            n = 200
            bases = 16 * rng.integers(0, (4096 - 16 * vectors) // 16, n)
            which = rng.integers(0, 8, n)
            steps = (Step * n)(*[Step(int(b), sets[w].ctypes.data) for b, w in zip(bases, which)])
            out = np.zeros(2 * n, np.int32)
            k.rx_mf(i.ctypes, q.ctypes, steps, ctypes.c_size_t(n), ctypes.c_uint(vectors), out.ctypes)
            for m, (b, w) in enumerate(zip(bases, which)):
                h = sets[w].astype(np.int32)
                assert out[2 * m] == np.dot(i[b:b + 16 * vectors].astype(np.int32), h)
                assert out[2 * m + 1] == np.dot(q[b:b + 16 * vectors].astype(np.int32), h)
    print("rx kernel references passed")


if __name__ == "__main__":
    main()
