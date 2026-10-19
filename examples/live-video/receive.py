#!/usr/bin/env python3
"""UHD or saved IQ -> LeanDVB -> MPEG-TS. Hardware requires explicit --args.

No device discovery is performed for --iq, --help or --plan.
"""
import argparse
import contextlib
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def commands(a, fifo="IQ_FIFO"):
    fmt = "s16" if a.args is not None else {"i16": "s16", "cf32": "f32"}.get(a.format, a.format)
    rx = [a.receiver, "--standard", "DVB-S2", "--" + fmt,
          "-f", str(a.rate), "--sr", str(a.symbol_rate),
          "--roll-off", str(a.rolloff), "--sampler", "rrc", "--rrc-rej", "20",
          "--modcods", str(1 << a.modcod), "--framesizes", "1",
          # Wider header search keeps lock through dummy-frame runs (see host/decode.py).
          "--ldpc-bf", "100", "--fastdrift", "--sr-tol", "0.0005", "--fd-info", "2", "-v"]
    source = None
    if a.args is not None:
        source = [a.uhd_rx_cfile, "--args", a.args, "--channels", "0",
                  "--samp-rate", str(a.rate), "--freq", str(a.freq),
                  "--gain", str(a.gain), "--antenna", a.antenna,
                  "--wire-format", "sc16", "--output-shorts", "--verbose",
                  "--nsamples", str(int(a.seconds * a.rate)), fifo]
    return source, rx


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run(a):
    # A FIFO keeps UHD's startup/status stdout separate from binary IQ.
    # In particular, piping uhd_rx_cfile /dev/stdout would corrupt the samples.
    source = decoder = player = None
    with tempfile.TemporaryDirectory(prefix="live-video-") as tmp, contextlib.ExitStack() as stack:
        fifo = str(Path(tmp) / "samples.s16")
        source_cmd, rx_cmd = commands(a, fifo)
        for cmd in (source_cmd, rx_cmd, shlex.split(a.player) if a.player else None):
            if cmd and not shutil.which(cmd[0]):
                raise ValueError(f"executable not found: {cmd[0]}")
        output = (sys.stdout.buffer if a.output == "-" else
                  stack.enter_context(open(a.output, "xb")))
        try:
            if source_cmd:
                os.mkfifo(fifo)
                # Nonblocking read lets us detect a failed UHD startup instead of
                # waiting forever for a writer to open the FIFO.
                fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                iq = stack.enter_context(os.fdopen(fd, "rb", buffering=0))
                source = subprocess.Popen(source_cmd, stdout=sys.stderr, stderr=sys.stderr)
                import select
                import time
                deadline = time.monotonic() + 30
                while not select.select([fd], [], [], .1)[0]:
                    if source.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("UHD failed to start IQ; see diagnostics")
                os.set_blocking(fd, True)
            else:
                iq = (sys.stdin.buffer if a.iq == "-" else
                      stack.enter_context(open(a.iq, "rb")))
            if a.player:
                player = subprocess.Popen(shlex.split(a.player), stdin=subprocess.PIPE,
                                          stdout=sys.stderr, stderr=sys.stderr)
            decoder = subprocess.Popen(rx_cmd, stdin=iq, stdout=subprocess.PIPE, stderr=sys.stderr)
            count = 0
            while data := decoder.stdout.read(188 * 256):
                output.write(data)
                if player:
                    player.stdin.write(data)
                count += len(data)
            decoder.stdout.close()
            output.flush()
            if player:
                player.stdin.close()
            codes = {"LeanDVB": decoder.wait()}
            if source:
                codes["UHD"] = source.wait(timeout=10)
            if player:
                codes["player"] = player.wait(timeout=10)
            if any(codes.values()) or count == 0 or count % 188:
                raise RuntimeError(f"receive failed: exits={codes}, TS bytes={count}")
            print(json.dumps({"ts_bytes": count, "exits": codes}), file=sys.stderr)
        finally:
            for process in (source, decoder, player):
                stop(process)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--iq", help="saved IQ file, or - for stdin")
    src.add_argument("--args", help="explicit UHD device args; this enables hardware access")
    p.add_argument("--receiver", default=os.environ.get("RX", "leandvb"))
    p.add_argument("--uhd-rx-cfile", default="uhd_rx_cfile")
    p.add_argument("--freq", type=float, help="receive center frequency in Hz; required with --args")
    p.add_argument("--gain", type=float, default=0, help="manual receive gain in dB (default 0)")
    p.add_argument("--antenna", default="RX2", help="select the physically connected RX port")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--rate", type=float, default=20e6)
    p.add_argument("--symbol-rate", type=float, default=10e6)
    p.add_argument("--rolloff", type=float, choices=(.20, .25, .35), default=.35)
    p.add_argument("--modcod", type=int, choices=range(4, 18), default=7)
    p.add_argument("--format", choices=("f32", "cf32", "s16", "i16"), default="f32", help="saved IQ format")
    p.add_argument("--output", default="-", help="new TS file, or - for stdout")
    p.add_argument("--player", help="player command reading TS on stdin (no shell)")
    p.add_argument("--plan", action="store_true", help="print commands without opening anything")
    a = p.parse_args()
    if any(not math.isfinite(v) or v <= 0 for v in (a.rate, a.symbol_rate, a.seconds)):
        p.error("rates and duration must be finite and positive")
    if a.rate < 2 * a.symbol_rate:
        p.error("LeanDVB requires at least 2 samples/symbol")
    if a.args is not None and (not a.args or a.freq is None or not math.isfinite(a.freq) or a.freq <= 0):
        p.error("hardware requires nonempty --args and a positive --freq")
    if not math.isfinite(a.gain):
        p.error("gain must be finite")
    if a.plan:
        source, rx = commands(a)
        print(json.dumps({"source": source or a.iq, "decoder": rx,
                          "output": a.output, "player": a.player}, indent=2))
        return
    try:
        run(a)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired, KeyboardInterrupt) as e:
        print(f"receive: {e}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
