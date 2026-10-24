"""Check the receiver's monitor datagrams (firmware/rx/main/mon_pack.c) against the format
host/monitor.py reads (exp/monitor): header <4sBBHI, samples <HBBI, symbols <HBB, stats JSON.

    python3 tests/test_mon_pack.py
"""
import ctypes as ct
import json
import os
import pathlib
import random
import shlex
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/rx/main"
HEADER, SAMPLES, SYMBOLS = struct.Struct("<4sBBHI"), struct.Struct("<HBBI"), struct.Struct("<HBB")

SHIM = r"""
#include "mon_pack.h"
size_t demo_stats(uint8_t *out, int full)
{
    mon_stats_t s = {.run = 3, .track = full, .freq_mhz = 2420, .symbol_rate = 5000000,
                     .modcod = 14, .gain = 32, .frames = 241163, .failed = 1, .lost = 2,
                     .relocks = 0, .dummies = 65303, .ldpc = 4, .runs_dropped = 30,
                     .frames_dropped = 1, .have_mer = full,
                     .mer_cdb = 2504, .mer_med_cdb = -5, .cfo_hz = -3479, .clock_ppb = -3091,
                     .ts = full, .packets = 1816705, .crc8 = 0, .cc = 46, .tei = 0,
                     .eth = full, .eth_sent = 259529, .eth_dropped = 510, .eth_q_peak = 256,
                     .mon_dropped = 7, .core0_pmil = full ? 664 : -1, .core1_pmil = -1,
                     .capture_drops = 0, .heap_after_boot = 0};
    return mon_stats(out, 0x1234, 0xDEADBEEF, &s);
}
"""


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    with tempfile.TemporaryDirectory(prefix="mon-") as tmp:
        shim = pathlib.Path(tmp) / "shim.c"
        shim.write_text(SHIM)
        lib_path = pathlib.Path(tmp) / "mon.so"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", "-I", str(MAIN),
            str(MAIN / "mon_pack.c"), str(shim), "-o", str(lib_path)], check=True)
        lib = ct.CDLL(str(lib_path))
        u8p, i8p = ct.POINTER(ct.c_uint8), ct.POINTER(ct.c_int8)
        lib.demo_stats.restype = ct.c_size_t
        lib.demo_stats.argtypes = [ct.c_void_p, ct.c_int]
        lib.mon_samples.restype = lib.mon_symbols.restype = ct.c_size_t
        lib.mon_samples.argtypes = [ct.c_void_p, ct.c_uint16, ct.c_uint32, ct.c_uint16, ct.c_uint,
                                    ct.c_uint, ct.c_char_p]
        lib.mon_symbols.argtypes = [ct.c_void_p, ct.c_uint16, ct.c_uint32, ct.c_uint, ct.c_uint,
                                    ct.c_char_p]
        lib.mon_unpack.argtypes = [ct.c_void_p, ct.POINTER(ct.c_uint32), ct.c_size_t]
        lib.mon_median.restype = ct.c_int16
        lib.mon_median.argtypes = [ct.POINTER(ct.c_int16), ct.c_uint]
        out = ct.create_string_buffer(1600)

        n = lib.demo_stats(out, 1)
        magic, version, kind, seq, t_us = HEADER.unpack_from(out.raw)
        check((magic, version, kind, seq, t_us) == (b"PVMN", 1, 1, 0x1234, 0xDEADBEEF), "header")
        body = out.raw[12:n]
        check(len(body) <= 1400, f"stats {len(body)} bytes")
        s = json.loads(body.decode())
        want = ["run", "state", "freq_mhz", "rate", "modcod", "short", "gain", "frames", "failed",
                "lost", "relocks", "dummies", "ldpc", "runs_dropped", "frames_dropped", "mer_db",
                "mer_med_db", "cfo_hz", "clock_ppm",
                "packets", "crc8", "cc", "tei", "eth_sent", "eth_dropped", "eth_q_peak",
                "mon_dropped", "core0_pct", "capture_drops", "heap_after_boot"]
        check(list(s) == want, f"keys {list(s)}")
        check(s["state"] == "track" and s["rate"] == "5m" and s["mer_db"] == 25.04 and
              s["mer_med_db"] == -0.05 and s["clock_ppm"] == -3.091 and s["cfo_hz"] == -3479 and
              s["core0_pct"] == 66.4 and s["eth_q_peak"] == 256 and s["runs_dropped"] == 30 and
              s["frames_dropped"] == 1, f"values {s}")
        n = lib.demo_stats(out, 0)
        s = json.loads(out.raw[12:n].decode())
        check(s["state"] == "acquire" and not {"mer_db", "packets", "eth_sent", "core0_pct",
                                               "core1_pct"} & set(s), f"unknowns left out {s}")

        rng = random.Random(4)
        iq = bytes(rng.getrandbits(8) for _ in range(1024))
        n = lib.mon_samples(out, 7, 99, 0xBEEF, 3, 4, iq)
        check(n == 1044 and HEADER.unpack_from(out.raw)[2:4] == (2, 7), "samples header")
        check(SAMPLES.unpack_from(out.raw, 12) == (0xBEEF, 3, 4, 16_000_000) and
              out.raw[20:n] == iq, "samples body")
        lib.mon_samples_planes.restype = ct.c_size_t
        lib.mon_samples_planes.argtypes = [ct.c_void_p, ct.c_uint16, ct.c_uint32, ct.c_uint16,
                                           ct.c_uint, ct.c_uint, ct.c_char_p, ct.c_char_p]
        si, sq = (bytes(rng.getrandbits(8) for _ in range(1024)) for _ in range(2))
        n = lib.mon_samples_planes(out, 9, 99, 5, 1, 2, si, sq)
        want = bytes(b for k in range(512, 1024) for b in (si[k], sq[k]))
        check(n == 1044 and SAMPLES.unpack_from(out.raw, 12) == (5, 1, 2, 16_000_000) and
              out.raw[20:n] == want, "samples from planes: chunk 1 of 2 interleaved")
        sym = bytes(rng.getrandbits(8) for _ in range(2 * 700))
        n = lib.mon_symbols(out, 8, 99, 700, 3, sym)
        check(n == 12 + 4 + 1280 and SYMBOLS.unpack_from(out.raw, 12) == (640, 3, 0) and
              out.raw[16:n] == sym[:1280], "symbols clamped to 640")

        # Dump words: I in bits 19:10, Q in 9:0, signed 10-bit; int8 is floor(s10 / 4).
        pairs = [(-512, 511), (-1, 1), (3, -4), (0, -3)] + [(rng.randint(-512, 511), rng.randint(-512, 511))
                                                           for _ in range(60)]
        words = (ct.c_uint32 * len(pairs))(*[(rng.getrandbits(12) << 20) | (i & 1023) << 10 | (q & 1023)
                                             for i, q in pairs])
        got = (ct.c_int8 * (2 * len(pairs)))()
        lib.mon_unpack(got, words, len(pairs))
        check(list(got) == [v >> 2 for p in pairs for v in p], "unpack")

        values = [rng.randint(-3000, 3000) for _ in range(33)]
        arr = (ct.c_int16 * 33)(*values)
        check(lib.mon_median(arr, 33) == sorted(values)[16], "median")
    print("monitor packing: header, stats JSON (unknowns left out), samples, symbols, unpack, median: ok")


if __name__ == "__main__":
    main()
