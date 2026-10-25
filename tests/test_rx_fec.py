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
                ("weak", ct.POINTER(ct.c_uint8)), ("header_sample", ct.c_uint64), ("sequence", ct.c_uint32),
                ("cfo_hz", ct.c_int32), ("clock_ppb", ct.c_int32), ("mer_cdb", ct.c_int16),
                ("modcod", ct.c_uint8), ("type", ct.c_uint8)]


class Stats(ct.Structure):
    _fields_ = [("iterations", ct.c_uint16), ("failed_checks", ct.c_uint16),
                ("unsatisfied", ct.c_uint16), ("ldpc_ok", ct.c_uint8), ("bch_ok", ct.c_uint8),
                ("header_ok", ct.c_uint8)]


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
        for name in ("rx_fec_size", "rx_fec_work_size"):
            getattr(self.lib, name).restype = ct.c_size_t
            getattr(self.lib, name).argtypes = [ct.c_uint]
        self.lib.rx_fec_init.argtypes = [ct.c_void_p, ct.c_uint, ct.c_void_p]
        self.lib.rx_fec_frame.argtypes = [ct.c_void_p, ct.POINTER(Frame),
                                          ct.POINTER(ct.POINTER(ct.c_uint8)), ct.POINTER(Stats)]
        self.decoders = {}
        return self

    def __exit__(self, *args):
        self.tmp.cleanup()

    def decoder(self, modcod, work=True):
        if (modcod, work) not in self.decoders:
            buffers = [ct.create_string_buffer(self.lib.rx_fec_size(modcod) + 16),
                       ct.create_string_buffer(self.lib.rx_fec_work_size(modcod) + 16)]
            ws, area = (ct.addressof(b) + (-ct.addressof(b) % 16) for b in buffers)
            assert self.lib.rx_fec_init(ws, modcod, area if work else None) == 0
            self.decoders[modcod, work] = (buffers, ws)
        return self.decoders[modcod, work][1]

    def frame(self, modcod, llr, hard_bits=None, frame_modcod=None, frame_type=3, work=True,
              weak_bits=None):
        """Decode one frame; returns (payload bytes or None, stats). With llr None, the hard
        and weak bits alone, as the board hands them over."""
        hard = np.zeros(2032, np.uint8)
        hard[:2025] = np.packbits(llr < 0 if hard_bits is None else hard_bits)
        weak = None
        if weak_bits is not None:
            weak = np.zeros(2032, np.uint8)
            weak[:2025] = np.packbits(weak_bits)
        if llr is not None:
            llr = np.ascontiguousarray(llr, np.int8)
        frame = Frame(None if llr is None else llr.ctypes.data_as(ct.POINTER(ct.c_int8)),
                      hard.ctypes.data_as(ct.POINTER(ct.c_uint8)),
                      None if weak is None else weak.ctypes.data_as(ct.POINTER(ct.c_uint8)),
                      0, 0, 0, 0, 0, modcod if frame_modcod is None else frame_modcod, frame_type)
        payload, stats = ct.POINTER(ct.c_uint8)(), Stats()
        n = self.lib.rx_fec_frame(self.decoder(modcod, work), ct.byref(frame), ct.byref(payload),
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


def int_model(llr, modcod, iterations=8, levels=None):
    """The integer decoder of rx_fec.c in codeword order (no slots, frames or vectors),
    with its stopping rule. Returns (hard bits or None, iterations). levels = (hard, weak)
    bits instead of llr: the two-level beliefs, 31 or 8 (WEAK), negated where hard."""
    sat, mag = lambda x: np.clip(x, -128, 127), lambda x: np.minimum(np.abs(x), 127)
    if levels is not None:
        b = np.where(levels[0], -1, 1) * np.where(levels[1], 8, 31).astype(np.int64)
    else:
        a = mag(llr[:16192].astype(np.int64))
        n, s1, s2 = len(a), int(a.sum()), int((a * a).sum())
        den = n * s2 - s1 * s1
        gain = min(64, max(1, (96 * s1 * n + den // 2) // den)) if den else 64
        b = np.where(llr < 0, -1, 1) * np.minimum(31, (mag(llr.astype(np.int64)) * gain) >> 4)
    layers = structure(modcod)
    R = [np.zeros(i.shape, np.int64) for i, _, _ in layers]
    before = [None, None]
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
        stuck = it >= 3 and (odd >= before[1] or 6 * odd > 360 * d.code(rate_of(modcod), True).q)
        before = [odd, before[0]]
        if (odd == 0 or stuck or it == iterations) and satisfied(layers, hard) == 0:
            return hard, it
        if stuck:
            return None, it
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
    # Without a work area: clean frames still pass, damaged ones return -1 undecoded.
    cw = codeword(7, rng.integers(0, 2, 800, dtype=np.uint8))
    assert native.frame(7, np.where(cw, -K, K), work=False)[0] is not None
    bad = cw.copy()
    bad[5] ^= 1
    payload, st = native.frame(7, np.where(cw, -K, K), hard_bits=bad, work=False)
    assert payload is None and st.failed_checks > 0 and st.iterations == 0
    print("clean: MODCOD 4/6/7/10, full, 100-byte and empty data fields, no iterations, "
          "no work area: ok")


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
    # Exact payloads where 8 iterations lose under 0.1% of frames.
    for modcod, esn0 in ((4, 2.75), (6, 4.5), (7, 5.25), (10, 7.25)):
        c = d.code(rate_of(modcod), True)
        data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
        cw, want = codeword(modcod, data), np.packbits(data).tobytes()
        for _ in range(20):
            llr, _ = demodulate(cw, esn0, rng)
            payload, st = native.frame(modcod, llr)
            assert payload == want and st.iterations > 0, (modcod, esn0)
    # Bit-exact Python model of the integer decoder on decoded and failed frames.
    for modcod, esn0 in ((7, 3.8), (7, 4.5), (5, 2.75), (9, 5.5), (10, 6.25)):
        c = d.code(rate_of(modcod), True)
        data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
        cw = codeword(modcod, data)
        llr, _ = demodulate(cw, esn0, rng)
        payload, st = native.frame(modcod, llr)
        hard, iterations = int_model(llr, modcod)
        assert st.iterations == iterations, (modcod, esn0, st.iterations, iterations)
        assert (payload is not None) == (hard is not None and (hard == cw).all())
    # FER against float layered BP on the same noise draws, both stopped after 8
    # iterations: in the waterfall of 3/4, the int8 decoder 0.25 dB higher loses no
    # more frames (with 30 iterations each the gap is 0.1 dB).
    modcod, frames, esn0, extra = 7, 60, 4.25, 0.25
    c = d.code(rate_of(modcod), True)
    data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
    cw, want = codeword(modcod, data), np.packbits(data).tobytes()
    seeds = rng.integers(0, 2**32, frames)
    natural = np.array([demodulate(cw, esn0, np.random.default_rng(x))[1] for x in seeds])
    fer_bp = np.mean((float_bp(natural, modcod, iterations=8)[:, :c.kbch] != cw[:c.kbch]).any(axis=1))
    fer_int = np.mean([native.frame(modcod, demodulate(cw, esn0 + extra,
                                                       np.random.default_rng(x))[0])[0] != want
                       for x in seeds])
    print(f"awgn: 3/4, {frames} frames: FER float BP {fer_bp:.3f} at {esn0} dB Es/N0, "
          f"int8 {fer_int:.3f} at {esn0 + extra} dB")
    assert fer_int <= fer_bp
    print("awgn: exact payloads above threshold, Python model bit exact, FER near float BP: ok")


def test_two_level(native):
    """The board's path: no soft bits, beliefs from the hard bits and the demodulator's weak
    bits (|soft| < 16, a point under half its nominal distance from the boundary). The decoder
    must match the Python model fed the same two levels, iteration for iteration, on frames
    it repairs and frames it gives up on, at three parity strides (q 25, 12 and 5)."""
    rng = np.random.default_rng(5)
    seen = {"decoded": 0, "failed": 0}
    for modcod, points in ((4, (1.5, 2.5, 4.0)), (7, (4.5, 5.5, 7.0)), (10, (6.5, 7.5, 9.0))):
        c = d.code(rate_of(modcod), True)
        for esn0 in points:
            data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
            cw, want = codeword(modcod, data), np.packbits(data).tobytes()
            for _ in range(3):
                soft, _ = demodulate(cw, esn0, rng, k=32)
                hard, weak = soft < 0, np.abs(soft.astype(int)) < 16
                payload, st = native.frame(modcod, None, hard_bits=hard, weak_bits=weak)
                model, iterations = int_model(None, modcod, levels=(hard, weak))
                if st.failed_checks == 0:
                    assert payload == want and st.iterations == 0, (modcod, esn0)
                    continue
                assert st.iterations == iterations, (modcod, esn0, st.iterations, iterations)
                assert (payload == want) == (model is not None and (model == cw).all()), (modcod, esn0)
                seen["decoded" if payload == want else "failed"] += 1
    assert seen["decoded"] and seen["failed"], seen
    print(f"two levels: hard and weak bits only, Python model bit exact on {seen['decoded']} "
          f"decoded and {seen['failed']} failed frames: ok")


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


def test_damage(native):
    """A good link hit by a glitch: confidently wrong bits are repaired; hopeless frames
    stop after 3 iterations."""
    rng = np.random.default_rng(6)
    modcod = 7
    c = d.code(rate_of(modcod), True)
    data = rng.integers(0, 2, c.kbch - 80, dtype=np.uint8)
    cw, want = codeword(modcod, data), np.packbits(data).tobytes()
    for wrong in (100, 200):
        llr = demodulate(cw, 15.0, rng)[0]
        flip = rng.choice(c.n, wrong, replace=False)
        llr[flip] = -llr[flip]
        payload, st = native.frame(modcod, llr)
        assert payload == want and st.iterations <= 8, wrong
    for llr in (rng.integers(-40, 41, c.n).astype(np.int8),
                np.where(rng.random(c.n) < 0.5, 0, demodulate(cw, 15.0, rng)[0]).astype(np.int8)):
        payload, st = native.frame(modcod, llr)
        assert payload is None and st.iterations == 3 and 6 * st.unsatisfied > 4320
    print("damage: 100 and 200 confidently wrong bits repaired, hopeless frames stop at 3: ok")


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


def test_ts(native):
    """Single-TS BBFRAMEs (host/dvbs2.py ts_bbframes) pass with their data fields exact; TS headers
    the receiver cannot deframe are rejected."""
    rng = np.random.default_rng(7)
    packets = rng.integers(0, 256, (60, 188), dtype=np.uint8)
    packets[:, 0] = 0x47
    for modcod in (4, 7, 10):
        rate = rate_of(modcod)
        frames = d.ts_bbframes(packets.ravel(), rate, short=True)
        assert len(frames) >= 4
        for bbf in frames:
            payload, st = native.frame(modcod, np.where(d.fecframe(bbf, rate, True), -K, K))
            assert payload == np.packbits(bbf[80:]).tobytes() and st.header_ok, modcod
    rate, modcod = rate_of(7), 7
    dfl = d.code(rate, True).kbch - 80
    data = rng.integers(0, 2, dfl, dtype=np.uint8)

    def frame(mt1=d.matype1(d.TS), upl=1504, sync=0x47, syncd=0, length=dfl):
        h = d.bbheader(mt1, length, upl, sync, syncd)
        return np.concatenate([h, data[:length], np.zeros(dfl - length, np.uint8)])

    for name, bbf, ok in (("TS", frame(), True), ("no packet start", frame(syncd=0xffff), True),
                          ("ACM", frame(mt1=d.matype1(d.TS, ccm=0)), True),
                          ("SYNCD at the end", frame(syncd=dfl - 8), True),
                          ("UPL", frame(upl=1512), False), ("SYNC", frame(sync=0x48), False),
                          ("SYNCD not bytes", frame(syncd=4), False),
                          ("SYNCD past the field", frame(syncd=dfl), False),
                          ("ISSY", frame(mt1=d.matype1(d.TS, issyi=1)), False),
                          ("null-packet deletion", frame(mt1=d.matype1(d.TS, npd=1)), False),
                          ("multiple streams", frame(mt1=d.matype1(d.TS, sis=0)), False)):
        payload, st = native.frame(modcod, np.where(d.fecframe(bbf, rate, True), -K, K))
        assert (payload is not None) == ok and st.bch_ok and st.header_ok == ok, name
    print("ts: ts_bbframes data fields exact at MODCOD 4/7/10, bad TS headers rejected: ok")


if __name__ == "__main__":
    with Native() as native:
        for test in (test_clean, test_failed_checks, test_awgn, test_two_level, test_int8_edges, test_damage,
                     test_false_accepts, test_ts):
            test(native)
