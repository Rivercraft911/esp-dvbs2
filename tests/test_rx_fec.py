"""Compile the receiver FEC (portable C) and check it against host/dvbs2.py frames.

Clean frames, AWGN frames against a float layered BP decoder written here in numpy,
a bit-exact Python model of the integer decoder, int8 edge cases and false accepts.
Run: python3 tests/test_rx_fec.py. No ESP-IDF, board, or pytest needed.
"""
import ctypes as ct
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(ROOT / "tests/reference"))
import dvbs2 as d  # noqa: E402
from gen_rx_fec import edges  # noqa: E402

COMPONENT = ROOT / "firmware/components/dvbs2rx"
K = 24                      # demodulator LLR units at the nominal constellation point
A = 1 / np.sqrt(2)          # QPSK amplitude per axis, unit symbol energy


class Frame(ct.Structure):
    _fields_ = [("llr", ct.POINTER(ct.c_int8)), ("hard", ct.POINTER(ct.c_uint8)),
                ("header_sample", ct.c_uint64), ("sequence", ct.c_uint32),
                ("cfo_hz", ct.c_int32), ("clock_ppb", ct.c_int32), ("mer_cdb", ct.c_int16),
                ("modcod", ct.c_uint8), ("type", ct.c_uint8)]


class Stats(ct.Structure):
    _fields_ = [("iterations", ct.c_uint16), ("failed_checks", ct.c_uint16),
                ("ldpc_ok", ct.c_uint8), ("bch_ok", ct.c_uint8), ("header_ok", ct.c_uint8)]


class Native:
    def __enter__(self):
        subprocess.run([sys.executable, str(ROOT / "tests/reference/gen_rx_fec.py"), "--check"],
                       check=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="rx-fec-")
        library = pathlib.Path(self.tmp.name) / "rx_fec.so"
        encoder = ROOT / "firmware/components/dvbs2"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            "-I", str(COMPONENT / "include"), "-I", str(COMPONENT), "-I", str(encoder / "include"),
            str(COMPONENT / "rx_fec.c"), str(encoder / "dvbs2.c"), str(encoder / "fast.c"),
            "-o", str(library)], check=True)
        self.lib = ct.CDLL(str(library))
        self.lib.rx_fec_size.restype = ct.c_size_t
        self.lib.rx_fec_size.argtypes = [ct.c_uint]
        self.lib.rx_fec_init.argtypes = [ct.c_void_p, ct.c_uint]
        self.lib.rx_fec_frame.argtypes = [ct.c_void_p, ct.POINTER(Frame),
                                          ct.POINTER(ct.POINTER(ct.c_uint8)), ct.POINTER(Stats)]
        self.decoders = {}
        return self

    def __exit__(self, *args):
        self.tmp.cleanup()

    def decoder(self, modcod):
        if modcod not in self.decoders:
            size = self.lib.rx_fec_size(modcod)
            raw = ct.create_string_buffer(size + 16)
            ws = ct.addressof(raw) + (-ct.addressof(raw) % 16)
            assert self.lib.rx_fec_init(ws, modcod) == 0
            self.decoders[modcod] = (raw, ws)
        return self.decoders[modcod][1]

    def frame(self, modcod, llr, hard_bits=None, frame_modcod=None, frame_type=3):
        """Decode one frame; returns (payload bytes or None, stats)."""
        llr = np.ascontiguousarray(llr, np.int8)
        hard = np.zeros(2032, np.uint8)
        hard[:2025] = np.packbits(llr < 0 if hard_bits is None else hard_bits)
        frame = Frame(llr.ctypes.data_as(ct.POINTER(ct.c_int8)),
                      hard.ctypes.data_as(ct.POINTER(ct.c_uint8)), 0, 0, 0, 0, 0,
                      modcod if frame_modcod is None else frame_modcod, frame_type)
        payload, stats = ct.POINTER(ct.c_uint8)(), Stats()
        n = self.lib.rx_fec_frame(self.decoder(modcod), ct.byref(frame), ct.byref(payload),
                                  ct.byref(stats))
        if n >= 0:
            start = ct.cast(payload, ct.c_void_p).value - hard.ctypes.data
            assert start == 10
            return bytes(hard[10:10 + n]), stats
        return None, stats


def rate_of(modcod):
    return d.RATES[modcod - 1]


def codeword(modcod, data):
    rate = rate_of(modcod)
    return d.fecframe(d.bbframe(data, rate, short=True), rate, short=True)


def demodulate(cw, esn0, rng, k=K):
    """BPSK-equivalent soft values with AWGN; int8 LLRs as a demodulator scales them
    (k units at the nominal point), and the natural LLRs."""
    sigma = np.sqrt(1 / (2 * 10 ** (esn0 / 10)))
    y = A * (1 - 2.0 * cw) + sigma * rng.standard_normal(cw.shape)
    return np.clip(np.round(k * y / A), -127, 127).astype(np.int8), 2 * A * y / sigma**2


def structure(modcod):
    """Per row group: codeword index of every edge's bit for each check t, and a mask
    that removes check 0's missing p_(-1)."""
    rate = rate_of(modcod)
    c = d.code(rate, True)
    groups, t = c.nbch // 360, np.arange(360)
    layers = []
    for r, row in enumerate(edges(rate)):
        idx = np.array([360 * g + (t - s) % 360 if g < groups else
                        c.nbch + g - groups + c.q * ((t - s) % 360) for g, s in row])
        mask = np.ones(idx.shape, bool)
        if r == 0:
            mask[-1, 0] = False
        layers.append((idx, mask, [g for g, _ in row]))
    return layers


def satisfied(layers, hard):
    """Unsatisfied checks of hard bits (batch on the first axis)."""
    return sum((np.bitwise_xor.reduce(np.where(m, hard[..., i], 0), axis=-2) != 0).sum(axis=-1)
               for i, m, _ in layers)


def float_bp(llr, modcod, iterations=50):
    """Float layered belief propagation (phi form), a batch of frames. Repeated groups in
    a row group add their message changes. Stops a frame when its signs are a codeword."""
    layers = structure(modcod)
    phi = lambda x: -np.log(np.tanh(np.clip(x, 1e-12, 50.0) / 2))  # noqa: E731
    b = llr.astype(float).copy()
    R = [np.zeros((len(b),) + i.shape) for i, _, _ in layers]
    done = np.zeros(len(b), bool)
    out = (b < 0).astype(np.uint8)
    for _ in range(iterations):
        for L, (idx, mask, _) in enumerate(layers):
            q = np.where(mask, b[:, idx] - R[L], 1e9)
            sign = np.where(q < 0, -1.0, 1.0)
            p = phi(np.abs(q))
            new = np.where(mask, sign.prod(axis=1, keepdims=True) * sign *
                           phi(p.sum(axis=1, keepdims=True) - p), 0.0)
            new[done] = R[L][done]
            np.add.at(b.T, idx.reshape(-1), (new - R[L]).reshape(len(b), -1).T)
            R[L] = new
        hard = (b < 0).astype(np.uint8)
        ok = ~done & (satisfied(layers, hard) == 0)
        out[ok], done = hard[ok], done | ok
        if done.all():
            break
    return out


def int_model(llr, modcod, iterations=30):
    """The integer decoder of rx_fec.c in codeword order (no slots, frames or vectors).
    Returns (hard bits or None, iterations)."""
    sat, mag = lambda x: np.clip(x, -128, 127), lambda x: np.minimum(np.abs(x), 127)
    a = mag(llr[:16192].astype(np.int64))
    n, s1, s2 = len(a), int(a.sum()), int((a * a).sum())
    den = n * s2 - s1 * s1
    gain = min(64, max(1, (96 * s1 * n + den // 2) // den)) if den else 64
    b = np.where(llr < 0, -1, 1) * np.minimum(63, (mag(llr.astype(np.int64)) * gain) >> 4)
    layers = structure(modcod)
    R = [np.zeros(i.shape, np.int64) for i, _, _ in layers]
    for it in range(1, iterations + 1):
        odd = 0
        for L, (idx, mask, groups) in enumerate(layers):
            w = np.where(mask, b[idx], 127)
            q = sat(w - R[L])
            m = np.sort(mag(q), axis=0)
            m1, m2 = m[0], m[1]
            neg = np.bitwise_xor.reduce(q, axis=0) < 0
            odd += (np.bitwise_xor.reduce(w, axis=0) < 0).sum()
            n1, n2 = np.minimum((m1 * 112) >> 7, 47), np.minimum((m2 * 112) >> 7, 47)
            sel = np.where(mag(q) < m2, np.where(neg, -n2, n2), np.where(neg, -n1, n1))
            msg = np.where(mask, np.where(q < 0, -sel, sel), 0)
            new, repeats = b.copy(), []
            for e, g in enumerate(groups):
                if g in groups[:e]:
                    repeats.append(e)
                else:
                    new[idx[e][mask[e]]] = sat(q[e] + msg[e])[mask[e]]
            for e in repeats:
                new[idx[e]] = sat(new[idx[e]] + sat(msg[e] - R[L][e]))
            b, R[L] = new, msg
        hard = (b < 0).astype(np.uint8)
        if (odd == 0 or it == iterations) and satisfied(layers, hard) == 0:
            return hard, it
    return None, iterations


# ---------------------------------------------------------------- tests

def test_clean(native):
    rng = np.random.default_rng(1)
    for modcod in (4, 6, 7, 10):
        c = d.code(rate_of(modcod), True)
        for length in (c.kbch - 80, 8 * 100, 0):
            data = rng.integers(0, 2, length, dtype=np.uint8)
            cw = codeword(modcod, data)
            for llr in (np.where(cw, -K, K), np.where(cw, -128, 127), np.where(cw, -1, 0)):
                payload, st = native.frame(modcod, llr, hard_bits=cw)
                assert payload == np.packbits(data).tobytes(), (modcod, length)
                assert (st.iterations, st.failed_checks, st.ldpc_ok, st.bch_ok, st.header_ok) == \
                    (0, 0, 1, 1, 1)
    print("clean: MODCOD 4/6/7/10, full, 100-byte and empty data fields, no iterations: ok")


def test_failed_checks(native):
    rng = np.random.default_rng(2)
    for modcod in (4, 7, 10):
        layers = structure(modcod)
        cw = codeword(modcod, rng.integers(0, 2, 800, dtype=np.uint8))
        for flips in (1, 2, 5, 40):
            bad = cw.copy()
            bad[rng.choice(len(cw), flips, replace=False)] ^= 1
            _, st = native.frame(modcod, np.where(cw, -K, K), hard_bits=bad)
            assert st.failed_checks == satisfied(layers, bad), (modcod, flips)
    print("failed_checks equals the unsatisfied checks of H on the received hard bits: ok")


def test_awgn(native):
    rng = np.random.default_rng(3)
    # Exact payloads 0.5 dB above each code's measured waterfall.
    for modcod, esn0 in ((4, 2.0), (6, 3.75), (7, 4.5), (10, 6.75)):
        c = d.code(rate_of(modcod), True)
        data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
        cw, want = codeword(modcod, data), np.packbits(data).tobytes()
        for _ in range(20):
            llr, _ = demodulate(cw, esn0, rng)
            payload, st = native.frame(modcod, llr)
            assert payload == want and st.iterations > 0, (modcod, esn0)
    # Bit-exact Python model of the integer decoder on decoded and failed frames.
    for modcod, esn0 in ((7, 3.8), (7, 4.5), (5, 2.5), (9, 5.25), (10, 6.25)):
        c = d.code(rate_of(modcod), True)
        data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
        cw = codeword(modcod, data)
        llr, _ = demodulate(cw, esn0, rng)
        payload, st = native.frame(modcod, llr)
        hard, iterations = int_model(llr, modcod)
        assert st.iterations == iterations, (modcod, esn0, st.iterations, iterations)
        assert (payload is not None) == (hard is not None and (hard == cw).all())
    # FER against float layered BP (50 iterations) on the same noise draws: in the
    # waterfall of 3/4, the int8 decoder 0.15 dB higher loses no more frames.
    modcod, frames, esn0, extra = 7, 60, 3.9, 0.15
    c = d.code(rate_of(modcod), True)
    data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
    cw, want = codeword(modcod, data), np.packbits(data).tobytes()
    seeds = rng.integers(0, 2**32, frames)
    natural = np.array([demodulate(cw, esn0, np.random.default_rng(x))[1] for x in seeds])
    fer_bp = np.mean((float_bp(natural, modcod)[:, :c.kbch] != cw[:c.kbch]).any(axis=1))
    fer_int = np.mean([native.frame(modcod, demodulate(cw, esn0 + extra,
                                                       np.random.default_rng(x))[0])[0] != want
                       for x in seeds])
    print(f"awgn: 3/4, {frames} frames: FER float BP {fer_bp:.3f} at {esn0} dB Es/N0, "
          f"int8 {fer_int:.3f} at {esn0 + extra} dB")
    assert fer_int <= fer_bp
    print("awgn: exact payloads above threshold, Python model bit exact, FER near float BP: ok")


def test_int8_edges(native):
    rng = np.random.default_rng(4)
    modcod = 7
    c = d.code(rate_of(modcod), True)
    data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
    cw, want = codeword(modcod, data), np.packbits(data).tobytes()
    # Saturated LLRs with errors: a few wrong bits at full confidence, -128 included.
    llr = np.where(cw, -128, 127).astype(np.int8)
    wrong = rng.choice(c.n, 12, replace=False)
    llr[wrong] = np.where(cw[wrong], 127, -128)
    llr[wrong[:6]] //= 8
    payload, st = native.frame(modcod, llr)
    assert payload == want and st.iterations > 0
    # Heavy demodulator gain clipped at +-127, and a tiny one: never a wrong payload.
    for k in (127, 2):
        for _ in range(5):
            payload, _ = native.frame(modcod, demodulate(cw, 5.0, rng, k)[0])
            assert payload in (want, None)
    # Erasures: half the frame at zero is too much, a short burst is not.
    llr = demodulate(cw, 6.0, rng)[0]
    burst = llr.copy()
    burst[3000:3400] = 0
    assert native.frame(modcod, burst)[0] == want
    llr[::2] = 0
    assert native.frame(modcod, llr)[0] is None
    assert native.frame(modcod, np.zeros(c.n, np.int8))[0] is None
    print("int8: saturated wrong bits, clipped and tiny scales, erasures: ok")


def test_false_accepts(native):
    rng = np.random.default_rng(5)
    modcod = 7
    rate = rate_of(modcod)
    c = d.code(rate, True)
    data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
    cw = codeword(modcod, data)
    # Random words, as hard bits with matching LLRs.
    for _ in range(10):
        llr = rng.integers(-127, 128, c.n).astype(np.int8)
        payload, st = native.frame(modcod, llr)
        assert payload is None and not st.ldpc_ok
    # Another MODCOD's frame, labelled as this MODCOD or as its own.
    other = codeword(6, rng.integers(0, 2, d.code(rate_of(6), True).kbch - 80, dtype=np.uint8))
    assert native.frame(modcod, np.where(other, -K, K))[0] is None
    assert native.frame(modcod, np.where(cw, -K, K), frame_modcod=6)[0] is None
    assert native.frame(modcod, np.where(cw, -K, K), frame_type=1)[0] is None   # normal frame

    def frame_from_bbframe(bbf, flip_after_bch=None):
        bch = d.bch_encode(d.bb_scramble(bbf), rate, short=True)
        if flip_after_bch is not None:
            bch = bch.copy()
            bch[flip_after_bch] ^= 1
        return d.ldpc_encode_groups(bch, rate, short=True)

    def header(mt1=0x70, dfl=c.kbch - 80, upl=0, syncd=0, crc_flip=0):
        b = d.int_bits(mt1 << 64 | upl << 40 | dfl << 24 | syncd, 72)
        return np.concatenate([b, d.int_bits(d.crc8(b) ^ crc_flip, 8)])

    def body(h, n=c.kbch - 80):
        return np.concatenate([h, data[:n], np.zeros(c.kbch - 80 - n, np.uint8)])

    good = frame_from_bbframe(body(header()))
    assert native.frame(modcod, np.where(good, -K, K))[0] == np.packbits(data).tobytes()
    cases = {
        "header CRC": body(header(crc_flip=0x10)),
        "TS stream type": body(header(mt1=0xf0)),
        "multiple streams": body(header(mt1=0x50)),
        "UPL": body(header(upl=188 * 8)),
        "SYNCD": body(header(syncd=5)),
        "DFL too long": body(header(dfl=c.kbch - 72)),
        "DFL not bytes": body(header(dfl=8 * 100 + 3), 8 * 100 + 3),
    }
    for name, bbf in cases.items():
        payload, st = native.frame(modcod, np.where(frame_from_bbframe(bbf), -K, K))
        assert payload is None and (st.ldpc_ok, st.bch_ok, st.header_ok) == (1, 1, 0), name
    # LDPC codewords whose BCH word is wrong: detected, never accepted.
    for flips in ([5], [100, 2000], list(range(0, 11000, 1000))):
        bad = frame_from_bbframe(body(header()), flip_after_bch=flips)
        payload, st = native.frame(modcod, np.where(bad, -K, K))
        assert payload is None and (st.ldpc_ok, st.bch_ok) == (1, 0), flips
        # The decoder converges to the same wrong LDPC codeword from noisy LLRs.
        payload, st = native.frame(modcod, demodulate(bad, 5.0, rng)[0])
        assert payload is None and not st.bch_ok
    print("false accepts: random words, wrong MODCOD and type, bad BBHEADERs, BCH errors: -1")


if __name__ == "__main__":
    with Native() as native:
        for test in (test_clean, test_failed_checks, test_awgn, test_int8_edges,
                     test_false_accepts):
            test(native)
