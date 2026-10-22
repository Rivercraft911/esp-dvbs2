"""C demodulator (firmware/components/dvbs2rx/demod.c) on a real excerpt and on the twin.

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
    d = R.Demod(so, link.modcod, not link.short, int(link.carrier_hz) // 1000)
    for k in range(0, len(words), chunk):
        d.feed(words[k:k + chunk])
    return d, words, truth


def main():
    subprocess.run([sys.executable, str(ROOT / "host/rx_demod.py"), "tables", "--check"], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        so = R.build(tmp)

        # Real dump (short7-gain32, 2420 MHz, 2026-10-22), 1,140 symbols from 150 before a
        # PLHEADER: acquisition, timing and carrier on the S3's own samples. The whole capture
        # gives 22 of 22 frames exact at CFO -3,267 to -3,291 Hz.
        d = R.Demod(so, 7, False, 2420000)
        d.feed(R.sc8_words(ROOT / "tests/data/rx_short7.sc8"), first=38280)
        s = d.stats()
        assert s["locked"] and s["rejects"] == 0 and not d.frames, s
        assert abs(s["cfo_hz"] + 3267) < 100 and abs(s["clock_ppb"] + 1350) < 50, s

        # Twin, QPSK 3/4 short frames at Es/N0 18 dB with the measured offsets and impairments,
        # and a 20 ms keyed-tone gap after frame 8: every frame exact, one relock.
        link = T.Link(frames=20, gap=(8, 0.02))
        d, words, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert len(pairs) == link.frames, (len(pairs), d.stats())
        assert all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)
        assert all((f["llr"] < 0).astype(np.uint8).tolist() == f["hard"].tolist() for f, _ in pairs[:2])
        # Sequence numbers count frame periods since the first lock, so the gap (2.4 periods)
        # reads as two lost frames.
        s = d.stats()
        assert s["relocks"] == 1 and s["lost"] == 2, s
        seq = [f["sequence"] for f, _ in pairs]
        assert seq == list(range(8)) + list(range(10, 22)), seq
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

        # A gap in the dump indices forces a relock and loses only the frame it cuts.
        d3 = R.Demod(so, 7)
        w, truth = T.twin(T.Link(frames=8, seed=7))
        cut = len(w) // 2
        d3.feed(w[:cut])
        d3.feed(w[cut + 5000:], first=cut + 5000)
        s = d3.stats()
        assert s["relocks"] == 1 and s["lost"] == 1 and len(d3.frames) == 7, (s, len(d3.frames))

        # Normal frames, as in the recorded corpus (QPSK 1/2, 2245 MHz offsets).
        link = T.Link(modcod=4, short=False, frames=3, cfo_hz=-11800, carrier_hz=2245e6, seed=5)
        d, _, truth = run_twin(so, link)
        pairs = matched(d, truth)
        assert len(pairs) == 3 and all(np.array_equal(f["hard"], t["codeword"]) for f, t in pairs)

    print("rx demodulator passed: real excerpt locked, twin frames exact, "
          f"implementation loss {loss:.2f} dB")


if __name__ == "__main__":
    main()
