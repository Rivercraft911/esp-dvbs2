"""Checks for the frame ring between the encoder and the bank fill (firmware/main/ring.c).

Run: python3 tests/test_ring.py. Compiles ring.c with tests/ring_host.c for the host and runs it
against a model of the radio_s3 bank schedule (see the comment in ring_host.c): a simulated
encoder at several speeds, then two real threads with random pauses. The harness itself fails on
any symbol that changes between reads, any read of a slot being written, and any frame out of
order or filler without a counted underrun. Timing on the board: dvbpace.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
L = 33282        # QPSK normal frame with pilots, symbols
SLOTS = 8        # RING_SLOTS in dvbtx.c


def run(exe, *args, sps=40, dummy=None, split=False, cache=False, packed=False, margin=None,
        fails=False, reuse=False, quads=False):
    out = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, env=dict(os.environ, RING_SPS=str(sps),
                         **({"RING_DUMMY": str(dummy)} if dummy else {}),
                         **({"RING_SPLIT": "1"} if split else {}),
                         **({"RING_CACHE": "1"} if cache else {}),
                         **({"RING_PACKED": "1"} if packed else {}),
                         **({"RING_QUADS": "1"} if quads else {}),
                         **({"RING_REUSE": "1", "RING_CODES": "1"} if reuse else {}),
                         **({"RING_CACHE_MARGIN": str(margin)} if margin is not None else {})))
    if fails:
        assert out.stdout.startswith("FAIL ring_window"), (args, out.stdout, out.stderr)
        return None
    assert out.returncode == 0 and out.stdout.startswith("ok"), (args, out.stdout, out.stderr)
    words = out.stdout.split()
    return {k: int(v) for k, v in zip(words[1::2], words[2::2])}


def test_ring():
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        print("ring: skipped, no host C compiler")
        return
    with tempfile.TemporaryDirectory() as tmp:
        (pathlib.Path(tmp) / "esp_attr.h").write_text("#define IRAM_ATTR\n")
        exe = pathlib.Path(tmp) / "ring_host"
        main = ROOT / "firmware" / "main"
        subprocess.run([cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                        f"-I{tmp}", f"-I{main}", str(main / "ring.c"),
                        str(ROOT / "tests" / "ring_host.c"), "-o", str(exe)], check=True)

        for sps in (3,4):
            for load in (400,700,1250):
                r = run(exe,"sim",L,3,400,load,17,3000,1,sps=sps,dummy=3330,
                        split=True,cache=True,packed=True,reuse=True,quads=True)
                assert r['underruns'] > 0
            run(exe,"threads",L,3,80,31,sps=sps,dummy=3330,
                split=True,cache=True,packed=True,reuse=True,quads=True)
        # The existing geometry below also exercises variable dummy choices.
        # The board's encoder: 2.97 M of 7.99 M cycles per frame at 1 Msym/s, 37 %.
        fast = run(exe, "sim", L, SLOTS, 120, 370, 0, 0, 1)
        assert fast["underruns"] == 0 and fast["frames"] == 120
        # The encoder refills a slot within a few bank periods of the fill leaving it, so more
        # than SLOTS - 2 whole frames are always ready beyond the fill.
        assert fast["min_lead"] > (SLOTS - 2) * L, fast

        slow = run(exe, "sim", L, SLOTS, 120, 1250, 0, 0, 1)      # 25 % too slow
        assert slow["underruns"] > 0 and slow["frames"] + slow["underruns"] == 120
        bursts = run(exe, "sim", L, SLOTS, 300, 370, 50, 12000, 1)  # 12-frame stall per 50
        assert bursts["underruns"] > 0
        cold = run(exe, "sim", L, SLOTS, 30, 370, 0, 0, 0)          # no frame ready at start
        assert cold["underruns"] >= 1
        short = run(exe, "sim", 3330, 4, 600, 900, 7, 3000, 1)       # 3330: dummy-frame length
        for sps in (4, 8, 40):
            length = 33282 if sps == 40 else 22194
            for load, prime in ((400, 1), (700, 1), (1250, 1), (1250, 0), (100000, 0)):
                r = run(exe, "sim", length, SLOTS, 80, load, 0, 0, prime, sps=sps, dummy=3330)
                assert (r["underruns"] == 0) if load < 1000 else (r["underruns"] > 0), r
            run(exe, "threads", length, SLOTS, 80, 23, sps=sps, dummy=3330)
        # Two separate internal slots: the 4-sps path. The encoder is observed only
        # once per bank here, a conservative delay after the actual release notification.
        for load in (400, 600, 700, 800, 1250):
            r = run(exe, "sim", 22194, 2, 500, load, 0, 0, 1, sps=4, dummy=3330,
                    split=True, cache=True)
            assert (r["underruns"] == 0) if load <= 800 else r["underruns"] > 0, r
        for load in (1250, 100000):
            r = run(exe, "sim", 22194, 2, 80, load, 0, 0, 0, sps=4, dummy=3330,
                    split=True, cache=True)
            assert r["underruns"] > 0
        r = run(exe, "sim", 22194, 2, 500, 600, 10, 3000, 1, sps=4, dummy=3330,
                split=True, cache=True)
        assert r["underruns"] > 0
        run(exe, "threads", 22194, 2, 120, 31, sps=4, dummy=3330, split=True, cache=True)
        # Packed internal slots (two phases per byte) at 4 and 8 sps: the 8PSK paths.
        for sps in (4, 8):
            for load in (400, 800):
                r = run(exe, "sim", 22194, 2, 300, load, 0, 0, 1, sps=sps, dummy=3330,
                        split=True, cache=True, packed=True)
                assert r["underruns"] == 0, r
            r = run(exe, "sim", 22194, 2, 200, 600, 10, 3000, 1, sps=sps, dummy=3330,
                    split=True, cache=True, packed=True)
            assert r["underruns"] > 0, r
            r = run(exe, "sim", 22194, 2, 40, 1250, 0, 0, 0, sps=sps, dummy=3330,
                    split=True, cache=True, packed=True)
            assert r["underruns"] > 0, r
            run(exe, "threads", 22194, 2, 80, 41 + sps, sps=sps, dummy=3330, split=True,
                cache=True, packed=True)
        # 10 Msym/s keeps three packed frames: two leave as little as 244 symbols of lead at
        # 70-80% encoder load and underrun at 90%; three hold 95%.
        two = run(exe, "sim", 22194, 2, 300, 800, 0, 0, 1, sps=4, dummy=3330, split=True,
                  cache=True, packed=True)
        for load in (800, 950):
            r = run(exe, "sim", 22194, 3, 300, load, 0, 0, 1, sps=4, dummy=3330, split=True,
                    cache=True, packed=True)
            assert r["underruns"] == 0 and r["min_lead"] > 50 * two["min_lead"], (r, two)
        run(exe, "threads", 22194, 3, 80, 47, sps=4, dummy=3330, split=True, cache=True,
            packed=True)
        # Finished-sample reuse makes fresh symbol reads monotonic, with ten history bytes.
        # Cross every output-wrap offset, both nibble parities, starvation and slot reuse.
        for sps, slots in ((4, 3), (8, 2)):
            for load in (400, 800, 950, 1250):
                r = run(exe, "sim", 22194, slots, 600, load, 0, 0, 1, sps=sps, dummy=3330,
                        split=True, cache=True, packed=True, reuse=True)
                limit = 950 if slots == 3 else 800
                assert (r["underruns"] == 0) if load <= limit else r["underruns"] > 0, r
            run(exe, "threads", 22194, slots, 120, 59, sps=sps, dummy=3330, split=True,
                cache=True, packed=True, reuse=True)
        # The cache's two end symbols are needed at 8 sps (bank edges cut symbols), not at 4.
        run(exe, "sim", 22194, 2, 20, 400, 0, 0, 1, sps=8, dummy=3330, split=True, cache=True,
            packed=True, margin=0, fails=True)
        run(exe, "sim", 22194, 2, 20, 400, 0, 0, 1, sps=4, dummy=3330, split=True, cache=True,
            packed=True, margin=0)
        # The 80 MS/s bank geometry (radio_s3.h: lead 128, tail 512) with the packed 8PSK ring
        # at 4 sps, the only rate it serves, and at 8 sps for the cut symbols.
        exe80 = pathlib.Path(tmp) / "ring_host_80m"
        subprocess.run([cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-DLEAD=128", "-DTAIL=512", f"-I{tmp}", f"-I{main}", str(main / "ring.c"),
                        str(ROOT / "tests" / "ring_host.c"), "-o", str(exe80)], check=True)
        for sps in (3,4):
            for load,burst in ((400,0),(800,0),(950,0),(700,17),(1250,0)):
                r = run(exe80,"sim",L,3,400,load,burst,3000,1,sps=sps,dummy=3330,
                        split=True,cache=True,packed=True,reuse=True,quads=True)
                assert (r['underruns'] == 0) if load <= 950 and not burst else r['underruns'] > 0
            run(exe80,"threads",L,3,80,31,sps=sps,dummy=3330,
                split=True,cache=True,packed=True,reuse=True,quads=True)
        for sps in (4, 8):
            for load in (400, 800, 950):
                r = run(exe80, "sim", 22194, 3, 300, load, 0, 0, 1, sps=sps, dummy=3330,
                        split=True, cache=True, packed=True)
                assert r["underruns"] == 0, r
            r = run(exe80, "sim", 22194, 3, 200, 600, 10, 3000, 1, sps=sps, dummy=3330,
                    split=True, cache=True, packed=True)
            assert r["underruns"] > 0, r
            run(exe80, "threads", 22194, 3, 80, 53 + sps, sps=sps, dummy=3330, split=True,
                cache=True, packed=True)
        run(exe80, "sim", 22194, 3, 20, 400, 0, 0, 1, sps=8, dummy=3330, split=True, cache=True,
            packed=True, margin=0, fails=True)
        threads = [run(exe, "threads", 3330, 4, 400, seed) for seed in (1, 2, 3)]
        threads.append(run(exe, "threads", L, SLOTS, 60, 7))
        assert sum(t["underruns"] for t in threads) > 0 and sum(t["frames"] for t in threads) > 0
    print(f"ring: model of the bank schedule; encoder at 37% keeps {fast['min_lead']} symbols "
          f"(> {SLOTS - 2} frames) ahead with no underrun; slow, bursty and cold-start encoders "
          f"give {slow['underruns']}, {bursts['underruns']} and {cold['underruns']} fillers; "
          f"short frames {short['underruns']}; two threads, {len(threads)} runs, "
          f"{sum(t['underruns'] for t in threads)} fillers; frames always in order, overlap "
          "re-reads identical, no read of a slot being written; packed ring also on the 80 MS/s "
          "bank geometry")


if __name__ == "__main__":
    test_ring()
