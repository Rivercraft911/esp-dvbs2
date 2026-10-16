"""Checks for the frame ring between the encoder and the bank fill (firmware/main/ring.c).

Run: python3 tests/test_ring.py. Compiles ring.c with tests/ring_host.c for the host and runs it
against a model of the radio_s3 bank schedule (see the comment in ring_host.c): a simulated
encoder at several speeds, then two real threads with random pauses. The harness itself fails on
any symbol that changes between reads, any read of a slot being written, and any frame out of
order or filler without a counted underrun. Timing on the board: dvbpace.
"""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
L = 33282        # QPSK normal frame with pilots, symbols
SLOTS = 8        # RING_SLOTS in dvbtx.c


def run(exe, *args):
    out = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
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
        threads = [run(exe, "threads", 3330, 4, 400, seed) for seed in (1, 2, 3)]
        threads.append(run(exe, "threads", L, SLOTS, 60, 7))
        assert sum(t["underruns"] for t in threads) > 0 and sum(t["frames"] for t in threads) > 0
    print(f"ring: model of the bank schedule; encoder at 37% keeps {fast['min_lead']} symbols "
          f"(> {SLOTS - 2} frames) ahead with no underrun; slow, bursty and cold-start encoders "
          f"give {slow['underruns']}, {bursts['underruns']} and {cold['underruns']} fillers; "
          f"short frames {short['underruns']}; two threads, {len(threads)} runs, "
          f"{sum(t['underruns'] for t in threads)} fillers; frames always in order, overlap "
          "re-reads identical, no read of a slot being written")


if __name__ == "__main__":
    test_ring()
