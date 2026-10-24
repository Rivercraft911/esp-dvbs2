"""C demodulator (firmware/components/dvbs2rx/demod.c) on real excerpts and on the twin.

Run: python3 tests/test_rx_demod.py. No board, ESP-IDF or pytest needed. The full captures are
not in the repository; host/rx_demod.py run reproduces those results.
"""
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import rx_demod as R  # noqa: E402
import rx_twin as T  # noqa: E402


def matched(demod, truth):
    """Frames paired with the twin's truth by header time: (frame, truth) list."""
    out = []
    for f in demod.frames:
        near = [t for t in truth if abs(t["header_sample"] - f["header_sample"]) < 32]
        assert near, f"frame at {f['header_sample']} matches no transmitted header"
        out.append((f, near[0]))
    return out


def run_twin(so, link, chunk=R.UNIT):
    words, truth = T.twin(link)
    d = R.Demod(so, link.modcod, not link.short, int(link.carrier_hz) // 1000, rate=link.rate,
                cfo_hint=-16600 if link.rate > 1e6 else -10000, cfo_span=40000 if link.rate > 1e6 else 30000)
    for k in range(0, len(words), chunk):
        d.feed(words[k:k + chunk])
    return d, words, truth


def fast(so):
    """5 Msym/s: a real 8PSK excerpt, QPSK and 8PSK short frames on the twin, the release."""
    # Real dump (normal13-5m-peak128: 8PSK 2/3 normal frames, 2420 MHz, 2026-10-22), 2,000
    # symbols from 150 before a PLHEADER, through the first pilot block. Fed from sample 0, the
    # first searched span; the whole capture gives 40 of 40 frames exact at CFO -13.6 kHz.
    d = R.Demod(so, 13, True, 2420000, quick_lock=True, rate=5e6)
    d.feed(R.sc8_words(ROOT / "tests/data/rx_5m13.sc8"), first=0)
    s = d.stats()
    assert s["locked"] and s["rejects"] == 0 and not d.frames, s
    assert abs(s["cfo_hz"] + 13597) < 100 and abs(s["clock_ppb"] + 5606) < 100, s

    # QPSK 3/4 with a 4 ms keyed tone after frame 6: every frame from the first lock to the
    # tone and from the relock on is exact, except the last symbols before the tone, which at
    # 5 Msym/s falls inside the matched filter's passband. The search covers a quarter of the
    # samples, so the relock may take a few frames.
    link = T.Link(rate=5e6, frames=24, gap=(6, 0.004))
    d, _, truth = run_twin(so, link)
    pairs = matched(d, truth)
    k = [truth.index(t) for _, t in pairs]
    assert k[:5] == list(range(1, 6)) and k[5:] == list(range(k[5], 24)) and k[5] <= 12, k
    errors = [np.flatnonzero(f["hard"] != t["codeword"]) for f, t in pairs]
    assert all(not len(e) for e in errors[:4] + errors[5:]) and all(errors[4] >= 16200 - 8), errors[4]
    assert d.stats()["relocks"] == 1 and d.stats()["dropped"] == 0

    # 8PSK 8/9 and 3/5 (its interleaver reads the columns in reverse) at 24 dB: exact, soft
    # bits agree with hard bits, few weak bits.
    for modcod in (16, 12):
        link = T.Link(rate=5e6, modcod=modcod, frames=8, mer_db=24, seed=modcod)
        d, words, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert len(pairs) == 7 and all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
        assert all(np.array_equal((f["llr"] < 0).astype(np.uint8), f["hard"]) for f, _ in pairs)
        assert all(f["weak"].mean() < 0.001 for f, _ in pairs)
        assert all(f["modcod"] == modcod and f["type"] == 3 for f, _ in pairs)

    # The monitor tap: 640 corrected symbols at radius 64 on the constellation (8PSK turned back
    # by pi/8; PL scrambling keeps QPSK and 8PSK points on their grids), outputs unchanged.
    so.rx_demod_tap.argtypes = [R.ct.c_void_p, R.ct.c_void_p, R.ct.c_void_p, R.ct.c_uint]
    so.rx_demod_tapped.restype = R.ct.c_uint
    so.rx_demod_tapped.argtypes = [R.ct.c_void_p, R.ct.c_void_p]
    so.rx_demod_tap_scale.argtypes = [R.ct.c_void_p, R.ct.c_void_p, R.ct.c_uint, R.ct.c_int16,
                                      R.ct.c_int, R.ct.c_void_p]
    for modcod, points, offset in ((16, 8, 0.0), (7, 4, np.pi / 4)):
        words, _ = T.twin(T.Link(rate=5e6, modcod=modcod, frames=6, mer_db=24, seed=modcod))
        runs = []
        for tap in (False, True):
            d = R.Demod(so, modcod, rate=5e6, cfo_hint=-16600, cfo_span=40000)
            ti, tq = (R.ct.c_int16 * 640)(), (R.ct.c_int16 * 640)()
            if tap:
                so.rx_demod_tap(R.ct.c_void_p(d.ptr), ti, tq, 640)
            for k in range(0, len(words), R.UNIT):
                d.feed(words[k:k + R.UNIT])
            runs.append((d, ti, tq))
        (plain, _, _), (tapped, ti, tq) = runs
        gain = R.ct.c_int16()
        assert so.rx_demod_tapped(R.ct.c_void_p(tapped.ptr), R.ct.byref(gain)) == 640 and gain.value > 0
        out = (R.ct.c_int8 * 1280)()
        so.rx_demod_tap_scale(ti, tq, 640, gain.value, int(modcod >= 12), out)
        buf = np.array(out, dtype=np.int8)
        assert len(plain.frames) == len(tapped.frames) > 0
        assert all(np.array_equal(a["llr"], b["llr"]) for a, b in zip(plain.frames, tapped.frames))
        z = buf[0::2].astype(float) + 1j * buf[1::2].astype(float)
        step = 2 * np.pi / points
        miss = (np.angle(z) - offset + step / 2) % step - step / 2
        assert abs(np.median(np.abs(z)) - 64) < 4 and np.percentile(np.abs(miss), 95) < 0.15, \
            (modcod, np.median(np.abs(z)), np.percentile(np.abs(miss), 95))

    # The monitor's sample snapshot: the newest 1,024 samples, as rx_unpack makes them (floor(s10
    # / 4)), wrapping the ring, after any push.
    so.rx_demod_snapshot.restype = R.ct.c_int64
    so.rx_demod_snapshot.argtypes = [R.ct.c_void_p, R.ct.c_void_p, R.ct.c_void_p, R.ct.c_size_t]
    d = R.Demod(so, 16, rate=5e6, cfo_hint=-16600, cfo_span=40000)
    si, sq = (R.ct.c_int8 * 1024)(), (R.ct.c_int8 * 1024)()
    assert so.rx_demod_snapshot(R.ct.c_void_p(d.ptr), si, sq, 2048) == -1   # over RX_SNAPSHOT_MAX
    assert so.rx_demod_snapshot(R.ct.c_void_p(d.ptr), si, sq, 1024) == -1   # nothing pushed
    fed = 0
    for n in (1000, 3001, 5000, 777):
        d.feed(words[fed:fed + n])
        fed += n
        first = so.rx_demod_snapshot(R.ct.c_void_p(d.ptr), si, sq, 1024)
        if fed < 1024:
            assert first == -1
            continue
        w = words[first:first + 1024].astype(np.uint32)
        i10 = ((w << 12).astype(np.int32) >> 22) >> 2
        q10 = ((w << 22).astype(np.int32) >> 22) >> 2
        assert first == fed - 1024 and np.array_equal(np.array(si), i10) and np.array_equal(np.array(sq), q10)

    # Implementation loss against the float reference receiver at 18 dB, 8PSK 8/9 (the raw
    # hard bits then have errors for the LDPC code to correct).
    link = T.Link(rate=5e6, modcod=16, frames=8, seed=6)
    d, words, truth = run_twin(so, link)
    pairs = matched(d, truth)
    ref = T.reference(words, truth[:7], link)
    loss = np.mean(ref[2:]) - np.mean([R.data_mer(f, t["codeword"], 16) for f, t in pairs[1:6]])
    assert loss < 0.5, loss

    # Dummy PLFRAMEs (MODCOD 0, 3,330 symbols, no pilots) from a transmitter whose input ran dry:
    # one, two, and 400 in a row (0.27 s) while the carrier drifts 2 kHz/s. The lock holds, every
    # data frame is exact and the sequence counts data frames only.
    dummies = (2, 4, 4) + (7,) * 400
    link = T.Link(rate=5e6, modcod=16, frames=12, mer_db=24, seed=8, dummies=dummies, drift_hz_s=-2000)
    d, _, truth = run_twin(so, link)
    pairs = matched(d, truth)
    s = d.stats()
    assert [truth.index(t) for _, t in pairs] == list(range(1, 12)) and s["dummies"] == 403, s
    assert all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
    assert [f["sequence"] for f, _ in pairs] == list(range(11)) and s["relocks"] == s["lost"] == 0, s

    # Push boundaries do not change the output while the release keeps up.
    d2, _, _ = run_twin(so, link, chunk=3001)
    assert len(d2.frames) == len(d.frames)
    assert all(np.array_equal(a["llr"], b["llr"]) for a, b in zip(d.frames, d2.frames))

    # A release that falls behind loses frames, never corrupts them: push 12,288-word units
    # with no release in between, as a stalled core 0 would.
    words, truth = T.twin(T.Link(rate=5e6, modcod=16, frames=8, mer_db=24, seed=4))
    d = R.Demod(so, 16, rate=5e6, cfo_hint=-16600, cfo_span=40000)
    for k in range(0, len(words), R.UNIT):
        d.so.rx_demod_push(R.ct.c_void_p(d.ptr), words[k:k + R.UNIT].ctypes.data, k, len(words[k:k + R.UNIT]))
        if k // R.UNIT % 3 == 2:
            d.release()
    pairs = matched(d, truth)
    assert d.stats()["dropped"] > 0 and all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
    return loss


def mf5_plan(so):
    """The 5 Msym/s batch plan (demod_mf5_plan, which the S3 build's demod_mf5_run uses) is the
    same whatever multiple of 2^26 samples the time carries, also where the five centres straddle
    2^27 samples, where 32-bit times in 1/32 sample wrap. The old 32-bit plan took that wrap for a
    window advance: two frames lost a batch of symbols on the board (results/2026-10-23-rx-no-wifi-ab)."""
    so.demod_mf5_plan.restype = R.ct.c_uint
    so.demod_mf5_plan.argtypes = [R.ct.c_uint64, R.ct.c_uint32, R.ct.POINTER(R.ct.c_uint32 * 5),
                                  R.ct.POINTER(R.ct.c_uint64)]

    def plan(t, step):
        c, b0 = (R.ct.c_uint32 * 5)(), R.ct.c_uint64()
        s = so.demod_mf5_plan(t, step, R.ct.byref(c), R.ct.byref(b0))
        return s, list(c), b0.value

    def old(t, step):                        # the 32-bit arithmetic it replaced
        c, b0, s = [], 0, 0
        for j in range(5):
            tq = ((t + j * step) >> 19) & 0xffffffff
            b = (((tq >> 5) - 8) & 0xffffffff) & ~15 & 0xffffffff
            b0 = b if j == 0 else b0
            s = j if not s and b != b0 else s
            c.append((tq - (b << 5)) & 0xffffffff)
        return s, c

    shift = (1 << 26) << 24                  # samples, Q24: a multiple of 16 and of the ring
    old_wrong = 0
    for step in (53687091, 53687091 + 160, 53687091 - 160):     # 3.2 samples, +-3 ppm
        for wrap in (1 << 27, 1 << 28, 3 << 27):
            for d in range(-14 << 24, 2 << 24, 1 << 21):      # first centre 14 samples before to 2 after
                t = (wrap << 24) + d
                s, c, b0 = plan(t, step)
                s2, c2, b2 = plan(t - shift, step)
                assert (s, c) == (s2, c2) and b0 - b2 == 1 << 26, (step, wrap, d)
                old_wrong += old(t, step) != (s, c)
    assert old_wrong > 0
    return old_wrong


def main():
    subprocess.run([sys.executable, str(ROOT / "host/rx_demod.py"), "tables", "--check"], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        so = R.build(tmp)

        # Real dump (short7-gain32, 2420 MHz, 2026-10-22), 1,140 symbols from 150 before a
        # PLHEADER: acquisition, timing and carrier on the S3's own samples. The whole capture
        # gives 22 of 22 frames exact at CFO -3,267 to -3,291 Hz.
        d = R.Demod(so, 7, False, 2420000, quick_lock=True)
        d.feed(R.sc8_words(ROOT / "tests/data/rx_short7.sc8"), first=38280)
        s = d.stats()
        assert s["locked"] and s["rejects"] == 0 and not d.frames, s
        assert abs(s["cfo_hz"] + 3267) < 100 and abs(s["clock_ppb"] + 1350) < 50, s

        # Twin, QPSK 3/4 short frames at Es/N0 18 dB with the measured offsets and impairments,
        # and a 20 ms keyed-tone gap after frame 8. Each acquisition locks on the header after
        # the one it finds, so frames 0 and 8 go to acquisition; the other 18 are exact.
        link = T.Link(frames=20, gap=(8, 0.02))
        d, words, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert [truth.index(t) for _, t in pairs] == list(range(1, 8)) + list(range(9, 20))
        assert all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
        assert all((f["llr"] < 0).astype(np.uint8).tolist() == f["hard"].tolist() for f, _ in pairs[:2])
        # Sequence numbers count frame periods since the first lock, so the gap (2.4 periods)
        # and the relock's frame read as three lost frames.
        s = d.stats()
        assert s["relocks"] == 1 and s["lost"] == 3, s
        seq = [f["sequence"] for f, _ in pairs]
        assert seq == list(range(7)) + list(range(10, 21)), seq
        assert all(f["modcod"] == 7 and f["type"] == 3 for f, _ in pairs)
        assert all(abs(f["cfo_hz"] - link.cfo_hz) < 60 for f, _ in pairs[:8])

        # Implementation loss against the float reference receiver, same samples.
        ref = T.reference(words, truth, link)
        loss = np.mean(ref) - np.mean([R.data_mer(f, t["codeword"]) for f, t in pairs])
        assert loss < 0.5, loss

        # Push boundaries do not change the output: odd-sized calls give identical LLRs.
        d2, _, _ = run_twin(so, link, chunk=4097)
        assert len(d2.frames) == len(d.frames)
        assert all(np.array_equal(a["llr"], b["llr"]) for a, b in zip(d.frames, d2.frames))

        # A gap in the dump indices forces a relock: it loses the frame it cuts and the one
        # the relock finds.
        d3 = R.Demod(so, 7)
        w, truth = T.twin(T.Link(frames=8, seed=7))
        cut = len(w) // 2
        d3.feed(w[:cut])
        d3.feed(w[cut + 5000:], first=cut + 5000)
        s = d3.stats()
        assert s["relocks"] == 1 and s["lost"] == 2 and len(d3.frames) == 5, (s, len(d3.frames))

        # A deferred lock whose header never comes (the keyed tone starts right after the
        # frame the search found) is rejected, and the search starts again at the newest
        # samples, not at the stale detection: it locks on the second frame after the tone.
        link = T.Link(frames=6, gap=(1, 0.05), seed=3)
        d, _, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert [truth.index(t) for _, t in pairs] == [2, 3, 4, 5], [truth.index(t) for _, t in pairs]
        assert all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
        assert d.stats()["rejects"] == 1, d.stats()

        # Dummy PLFRAMEs between data frames ride through at 1 Msym/s too: one, two, then 100 in
        # a row (0.33 s), at 18 dB. The search finds frame 0 and defers the lock to the next
        # header, a dummy's: the lock hops over it to frame 1.
        link = T.Link(frames=10, dummies=(0, 1, 3, 5, 5) + (8,) * 100, seed=9)
        d, _, truth = run_twin(so, link)
        pairs = matched(d, truth)
        s = d.stats()
        assert [truth.index(t) for _, t in pairs] == list(range(1, 10)), [truth.index(t) for _, t in pairs]
        assert all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
        assert s["dummies"] == 105 and s["relocks"] == s["lost"] == s["rejects"] == 0, s
        assert [f["sequence"] for f, _ in pairs] == list(range(9))

        # A transmitter starved to an eighth of its rate (seven dummies after every frame) at
        # 15 dB: the lock hops from dummy to dummy to the next frame and every frame is exact.
        link = T.Link(frames=8, dummies=tuple(k for k in range(8) for _ in range(7)), mer_db=15, seed=11)
        d, _, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert [truth.index(t) for _, t in pairs] == list(range(1, 8)), [truth.index(t) for _, t in pairs]
        assert all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
        assert d.stats()["rejects"] == d.stats()["relocks"] == 0, d.stats()

        # Normal frames, as in the recorded corpus (QPSK 1/2, 2245 MHz offsets).
        link = T.Link(modcod=4, short=False, frames=3, cfo_hz=-11800, carrier_hz=2245e6, seed=5)
        d, _, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert len(pairs) == 2 and all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)

        loss5 = fast(so)
        wrong = mf5_plan(so)

    print("rx demodulator passed: real excerpts locked, twin frames exact, "
          f"implementation loss {loss:.2f} dB at 1 Msym/s QPSK, {loss5:.2f} dB at 5 Msym/s 8PSK, "
          f"5 Msym/s plan unchanged across 2^27-sample wraps (the old one wrong at {wrong} times)")


if __name__ == "__main__":
    main()
