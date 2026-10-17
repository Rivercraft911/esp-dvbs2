#!/usr/bin/env python3
"""File-only LeanDVB throughput/noise benchmark. RTF = signal seconds / wall seconds.

AWGN is generated before timing. Es/N0 = sample SNR + 10 log10(Fs/Rs).
Repeat short captures continuously, preserving their I/Q order (loop joins are not RF).
"""
import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import subprocess
import tempfile
import time

import numpy as np
from scipy.signal import resample_poly

import decode


def receiver_command(executable, fs, rs, *, fmt="f32", sampler="rrc", flips=200,
                     resample=False):
    cmd = [str(executable), "--standard", "DVB-S2", "--" + fmt, "-f", str(fs),
           "--sr", str(rs), "--sampler", sampler, "--rrc-rej", "20", "--fastdrift",
           "--roll-off", "0.35", "--ldpc-bf", str(flips), "--fd-info", "2", "-d"]
    return cmd + (["--resample"] if resample else [])


def add_noise(z, fs, rs, esn0, seed):
    if esn0 is None:
        return z.copy()
    rng = np.random.default_rng(seed)
    variance = float(np.mean(abs(z)**2)) * fs / rs * 10**(-esn0 / 10)
    noise = rng.standard_normal((len(z), 2)).astype(np.float32)
    return z + noise.view(np.complex64).ravel() * np.sqrt(variance / 2)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture", type=Path)
    p.add_argument("--firmware", default="unknown", help="capture firmware revision, if recorded")
    p.add_argument("--receiver", default=os.environ.get("LEANDVB", str(decode.DEFAULT_RECEIVER)))
    p.add_argument("--sample-rate", type=int, required=True)
    p.add_argument("--symbol-rate", type=int, required=True)
    p.add_argument("--format", choices=("s16", "f32"), default="s16")
    p.add_argument("--wire-format", choices=("f32", "s16"), default="f32")
    p.add_argument("--scale", type=float, default=512)
    p.add_argument("--seconds", type=float, default=3)
    p.add_argument("--esn0", default="clean,21,18,15,12,9.62")
    p.add_argument("--flips", type=int, default=200)
    p.add_argument("--sampler", choices=("rrc", "linear"), default="rrc")
    p.add_argument("--decimate", type=int, default=1, help="offline FIR, timed separately")
    p.add_argument("--resample", action="store_true", help="LeanDVB timed FIR preprocessing")
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--out", type=Path, required=True, help="new JSONL, refuses overwrite")
    args = p.parse_args()
    if (args.sample_rate < 2 * args.symbol_rate or args.seconds <= 0 or args.repeat < 1
            or args.decimate < 1 or args.sample_rate % args.decimate or args.scale <= 0):
        p.error("invalid rates, duration, scale, repeat or decimation")
    fs = args.sample_rate // args.decimate
    if fs < 2 * args.symbol_rate:
        p.error("decimated input must retain >=2 samples/symbol")
    if args.capture.stat().st_size % (4 if args.format == "s16" else 8):
        p.error("partial I/Q sample")
    raw = np.fromfile(args.capture, "<i2" if args.format == "s16" else "<c8")
    z = raw.astype(np.float32).view(np.complex64) / args.scale if args.format == "s16" else raw
    t = time.perf_counter()
    if args.decimate > 1:
        z = resample_poly(z, 1, args.decimate).astype(np.complex64)
    prep = time.perf_counter() - t
    loops = math.ceil(args.seconds * fs / len(z))
    revision = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], capture_output=True, text=True, check=True).stdout)
    provenance = dict(host_revision=revision, host_dirty=dirty, capture_firmware=args.firmware, capture=str(args.capture.resolve()), capture_sha256=hashlib.sha256(
        args.capture.read_bytes()).hexdigest(), receiver_sha256=hashlib.sha256(
        Path(args.receiver).read_bytes()).hexdigest(), platform=platform.platform(),
        cpu_count=os.cpu_count(), input_sample_rate=args.sample_rate, sample_rate=fs,
        symbol_rate=args.symbol_rate, samples_per_loop=len(z), loops=loops,
        signal_seconds=loops * len(z) / fs, offline_resample_seconds=prep,
        decimate=args.decimate, seed=20261017, clock="perf_counter", scope="file loop, not RF")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("x") as output, tempfile.TemporaryFile() as source:
        for es in args.esn0.split(","):
            esn0 = None if es == "clean" else float(es)
            noisy = add_noise(z, fs, args.symbol_rate, esn0, 20261017)
            source.seek(0)
            source.truncate()
            if args.wire_format == "s16":
                block = np.rint(noisy.view(np.float32)*2048).clip(-32768,32767).astype("<i2").tobytes()
            else:
                block = noisy.astype("<c8").tobytes()
            for _ in range(loops):
                source.write(block)
            source.flush()
            for trial in range(args.repeat):
                source.seek(0)
                cmd = receiver_command(args.receiver, fs, args.symbol_rate,
                                       sampler=args.sampler, flips=args.flips, resample=args.resample, fmt=args.wire_format)
                with tempfile.TemporaryFile() as log:
                    before = resource.getrusage(resource.RUSAGE_CHILDREN)
                    start = time.perf_counter()
                    done = subprocess.run(cmd, stdin=source, stdout=subprocess.DEVNULL,
                                          stderr=log, timeout=180)
                    wall = time.perf_counter() - start
                    after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    log.seek(0)
                    status = log.read().decode(errors="replace")
                fec = [tuple(map(int, m)) for m in decode.FEC_STATUS.findall(status)]
                mer = [float(x) for x in re.findall(r"(?<![A-Za-z])MER ([\d.-]+)", status)]
                row = dict(**provenance, measured_at=dt.datetime.now(dt.timezone.utc).isoformat(), trial=trial, esn0_db=esn0, command=cmd,
                           wall_seconds=wall, rtf=provenance["signal_seconds"] / wall,
                           user_seconds=after.ru_utime-before.ru_utime,
                           system_seconds=after.ru_stime-before.ru_stime,
                           returncode=done.returncode, fec_attempts=len(fec),
                           frames_ok=sum(a[6] >= 0 for a in fec), frames_failed=sum(a[6] < 0 for a in fec),
                           ldpc_flips=sum(a[3] for a in fec),
                           ldpc_residual_frames=sum(a[5] > 0 for a in fec),
                           mer_db_median=float(np.median(mer)) if mer else None,
                           bad_bbheaders=status.count("Bad bbframe"), status_bytes=len(status))
                output.write(json.dumps(row, allow_nan=False) + "\n")
                output.flush()
                print(json.dumps({k: row[k] for k in ("esn0_db", "trial", "rtf", "frames_ok",
                                                      "frames_failed", "mer_db_median")}), flush=True)
                if done.returncode:
                    raise SystemExit(done.returncode)


if __name__ == "__main__":
    main()
