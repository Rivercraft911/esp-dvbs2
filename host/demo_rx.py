#!/usr/bin/env python3
"""One-screen file/live DVB-S2 receiver. Binds only to localhost. No serial access.

    python3 host/demo_rx.py --demo
    python3 host/demo_rx.py --file dump.iq --sample-rate 40000000 --symbol-rate 10000000
    producer | python3 host/demo_rx.py --stdin --sample-rate 20000000 --symbol-rate 10000000

E200 access happens ONLY with --e200, for the coordinator to run.
"""
import argparse
from collections import deque
import json
import math
import os
from pathlib import Path
import queue
import re
import select
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
import webbrowser

import numpy as np
import decode
from rxbench import receiver_command
from rx_rate import stream_command
from rx_fixture import ffmpeg_path, make_fixture


class State:
    def __init__(self, args):
        self.lock = threading.RLock()
        self.started = time.monotonic()
        self.ts_history = deque()
        self.data = dict(source="FILE REPLAY" if args.file else "LIVE INPUT", status="starting",
                         sample_rate=args.sample_rate, symbol_rate=args.symbol_rate,
                         center_hz=args.center, modcod=None, mer_db=None, frames_ok=0,
                         frames_failed=0, bbheader_failed=0, frame_lock=False, ts_bytes=0,
                         iq_samples=0, video_frames=0, spectrum_seq=0, spectrum=[],
                         source_loops=0, error=None, tx=None, tx_age_s=None)
        self.jpeg = b""
        self.video_at = self.mer_at = self.fec_at = self.tx_at = None
        self.spectrum_at = None
        self.paced = args.file is not None and not args.unpaced

    def status_line(self, line):
        with self.lock:
            match = decode.FEC_STATUS.search(line)
            if match:
                mc, _, _, _, _, _, bch = map(int, match.groups())
                self.data["modcod"] = mc
                self.data["frames_failed" if bch < 0 else "frames_ok"] += 1
                self.fec_at = time.monotonic()
            match = re.search(r"(?<![A-Za-z])MER ([\d.-]+)", line)
            if match and math.isfinite(float(match[1])):
                self.data["mer_db"] = float(match[1])
                self.mer_at = time.monotonic()
            match = re.search(r"(?<![A-Za-z])FRAMELOCK ([01])", line)
            if match:
                self.data["frame_lock"] = bool(int(match[1]))
            self.data["bbheader_failed"] += line.count("Bad bbframe")

    def count_ts(self, count):
        with self.lock:
            self.data["ts_bytes"] += count
            self.ts_history.append((time.monotonic(), self.data["ts_bytes"]))
            while len(self.ts_history) > 2 and self.ts_history[1][0] < time.monotonic() - 2:
                self.ts_history.popleft()

    def snapshot(self):
        with self.lock:
            now = time.monotonic()
            d = dict(self.data)
            elapsed = now - self.started
            d["elapsed_s"] = elapsed
            d["input_msps"] = d["iq_samples"] / max(elapsed, .001) / 1e6
            d["input_ratio"] = d["iq_samples"] / d["sample_rate"] / max(elapsed, .001)
            d["behind_s"] = max(0, elapsed - d["iq_samples"] / d["sample_rate"])
            # Use wall time, not the claimed symbol rate or a fixed payload capacity.
            d["received_mbps"] = 0
            if len(self.ts_history) > 1 and now-self.ts_history[-1][0] < 2:
                first = self.ts_history[0]
                d["received_mbps"] = (d["ts_bytes"] - first[1]) * 8 / max(now-first[0], .001) / 1e6
            for key, stamp in (("video_age_s", self.video_at), ("mer_age_s", self.mer_at),
                               ("fec_age_s", self.fec_at), ("tx_age_s", self.tx_at),
                               ("spectrum_age_s", self.spectrum_at)):
                d[key] = now-stamp if stamp is not None else None
            d["mode"] = decode.MODCODS.get(d["modcod"], "acquiring")
            d["healthy"] = bool(d["frame_lock"] and self.fec_at and now-self.fec_at < 2
                                and not d["error"] and d["status"] == "running")
            return d


def spectrum_db(raw, fmt, scale=1, nfft=1024):
    if fmt == "s16":
        z = np.frombuffer(raw, "<i2").astype(np.float32).view(np.complex64) / scale
    else:
        z = np.frombuffer(raw, "<c8")
    if len(z) < nfft:
        raise ValueError("spectrum block too short")
    window = np.hanning(nfft)
    # dBFS per FFT bin, coherent-gain normalized. Not calibrated dBm or MER.
    power = abs(np.fft.fftshift(np.fft.fft(z[:nfft] * window)))**2 / window.sum()**2
    return np.round(10*np.log10(np.maximum(power, 1e-12)), 1).tolist()


def jpeg_frames(stream):
    buf = bytearray()
    while block := stream.read(65536):
        buf.extend(block)
        while True:
            first = buf.find(b"\xff\xd8")
            end = buf.find(b"\xff\xd9", first+2) if first >= 0 else -1
            if end < 0:
                if len(buf) > 16*1024*1024:
                    raise ValueError("invalid oversized JPEG stream")
                break
            yield bytes(buf[first:end+2])
            del buf[:end+2]


def read_blocks(source, size, stop):
    """Keep partial pipe reads together; polling makes idle stdin stoppable."""
    pending = bytearray()
    fd = source.fileno()
    while not stop.is_set():
        if not select.select([fd], [], [], .2)[0]:
            continue
        block = os.read(fd, size-len(pending))
        if not block:
            if pending:
                yield bytes(pending)
            return
        pending.extend(block)
        if len(pending) == size:
            yield bytes(pending)
            pending.clear()


class Receiver:
    def __init__(self, args, state):
        self.args, self.state = args, state
        self.stop = threading.Event()
        self.spectra = queue.Queue(maxsize=1)
        self.processes, self.threads = [], []
        self.log = None

    def thread(self, fn):
        def guarded():
            try:
                fn()
            except Exception as exc:
                if not self.stop.is_set():
                    with self.state.lock:
                        self.state.data.update(error=f"{type(exc).__name__}: {exc}", status="failed")
                    self.stop.set()
        t = threading.Thread(target=guarded, daemon=True)
        self.threads.append(t)
        t.start()

    def start(self):
        a, s = self.args, self.state
        ff = [ffmpeg_path(), "-hide_banner", "-loglevel", "warning", "-threads", "2",
              "-fflags", "+discardcorrupt", "-probesize", "32768", "-analyzeduration", "0",
              "-f", "mpegts", "-i", "pipe:0", "-map", "0:v:0", "-an", "-vf",
              "scale='min(1280,iw)':-2", "-fps_mode", "passthrough", "-c:v", "mjpeg",
              "-threads", "2", "-q:v", "4", "-f", "image2pipe", "-flush_packets", "1", "pipe:1"]
        self.video = subprocess.Popen(ff, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, bufsize=-1)
        self.processes.append(self.video)
        cmd = receiver_command(a.receiver, a.sample_rate, a.symbol_rate, fmt=a.format,
                               flips=a.flips)
        if a.tune:
            cmd += ["--tune", str(a.tune)]
        self.decoder = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=-1)
        self.processes.append(self.decoder)
        if a.log:
            self.log = a.log.open("x")
        self.thread(self.status)
        self.thread(self.transport)
        self.thread(self.images)
        self.thread(self.fft)
        self.thread(self.video_errors)
        if a.telemetry:
            self.thread(self.telemetry)
        self.thread(self.input)

    def status(self):
        for line in self.decoder.stderr:
            text = line.decode(errors="replace")
            self.state.status_line(text)
            if self.log:
                self.log.write(text)

    def video_errors(self):
        self.video_log = deque(maxlen=12)
        for line in self.video.stderr:
            self.video_log.append(line.decode(errors="replace").strip())

    def transport(self):
        output = self.args.ts_out.open("xb") if self.args.ts_out else None
        try:
            while raw := self.decoder.stdout.read(188*256):
                self.state.count_ts(len(raw))
                if output:
                    output.write(raw)
                # Blocking here propagates video backpressure to input, never hides IQ drops.
                self.video.stdin.write(raw)
            self.video.stdin.close()
        finally:
            if output:
                output.close()

    def images(self):
        for jpeg in jpeg_frames(self.video.stdout):
            with self.state.lock:
                self.state.jpeg = jpeg
                self.state.video_at = time.monotonic()
                self.state.data["video_frames"] += 1

    def fft(self):
        while not self.stop.is_set():
            try:
                raw = self.spectra.get(timeout=.2)
            except queue.Empty:
                continue
            power = spectrum_db(raw, self.args.format, self.args.scale)
            with self.state.lock:
                self.state.data["spectrum"] = power
                self.state.data["spectrum_seq"] += 1
                self.state.spectrum_at = time.monotonic()

    def telemetry(self):
        # A regular JSONL file supplied by the coordinator, never a serial device.
        with self.args.telemetry.open() as stream:
            while not self.stop.is_set():
                line = stream.readline()
                if not line:
                    self.stop.wait(.2)
                    continue
                try:
                    row = json.loads(line)
                    fields = {k: float(row[k]) for k in ("fill_slack_us", "encoder_load_pct") if k in row}
                    if not fields or not all(math.isfinite(v) for v in fields.values()):
                        continue
                except (ValueError, TypeError):
                    continue
                with self.state.lock:
                    self.state.data["tx"] = fields
                    self.state.tx_at = time.monotonic()

    def input(self):
        a, s = self.args, self.state
        source = None
        try:
            if a.e200:
                import sdr
                radio = sdr.SDR(sdr.Settings(uri=a.uri, lo_hz=int(a.center), rate_hz=a.sample_rate,
                                            bandwidth_hz=a.bandwidth, gain_db=a.gain), a.libiio)
                metadata = radio.configure()
                # One and only one RX process. Both I and Q, single RF channel, no -s limit.
                cmd = stream_command(radio)
                if a.log:
                    self.log.write(json.dumps({"e200": metadata, "command": cmd}) + "\n")
                self.source_process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                       env=radio.env, bufsize=0)
                self.processes.append(self.source_process)
                self.thread(self.source_errors)
                source = self.source_process.stdout
            else:
                source = a.file.open("rb") if a.file else sys.stdin.buffer
            s.started = time.monotonic()
            s.data["status"] = "running"
            width = 4 if a.format == "s16" else 8
            last_fft = 0
            while not self.stop.is_set():
                for block in read_blocks(source, 262144*width, self.stop):
                    if len(block) % width:
                        raise ValueError("input ends in a partial I/Q sample")
                    now = time.monotonic()
                    if now-last_fft >= .1 and len(block) >= 1024*width:
                        try:
                            self.spectra.put_nowait(block[:1024*width])
                        except queue.Full:
                            pass
                        last_fft = now
                    # Write all bytes: unbuffered pipe writes can be short.
                    view = memoryview(block)
                    while view and not self.stop.is_set():
                        ready = select.select([], [self.decoder.stdin], [], .2)[1]
                        if ready:
                            view = view[os.write(self.decoder.stdin.fileno(), view[:65536]):]
                    with s.lock:
                        s.data["iq_samples"] += len(block)//width
                    if s.paced:
                        self.stop.wait(max(0, s.data["iq_samples"]/a.sample_rate-(time.monotonic()-s.started)))
                    if self.stop.is_set():
                        break
                if a.file and a.loop and not self.stop.is_set():
                    source.seek(0)
                    with s.lock:
                        s.data["source_loops"] += 1
                else:
                    break
            self.decoder.stdin.close()
            if not self.stop.is_set():
                if a.e200 and self.source_process.wait(timeout=6):
                    raise RuntimeError("E200 stream exited with an error; inspect receiver log")
                code = self.decoder.wait(timeout=30)
                if code:
                    raise RuntimeError(f"LeanDVB exited {code}")
                with s.lock:
                    s.data["status"] = "ended"
                    s.data["frame_lock"] = False
        finally:
            if source and source is not sys.stdin.buffer:
                source.close()

    def source_errors(self):
        for line in self.source_process.stderr:
            if self.log:
                self.log.write("E200 " + line.decode(errors="replace"))

    def close(self):
        self.stop.set()
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
        for process in self.processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for thread in self.threads:
            thread.join(timeout=1)
        if self.log:
            self.log.close()
        with self.state.lock:
            if self.state.data["status"] == "running":
                self.state.data["status"] = "stopped"
            self.state.data["frame_lock"] = False


def handler(state):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/":
                body, kind = Path(__file__).with_suffix(".html").read_bytes(), "text/html; charset=utf-8"
            elif path == "/api/state":
                body, kind = json.dumps(state.snapshot(), allow_nan=False).encode(), "application/json"
            elif path == "/frame.jpg":
                with state.lock:
                    body = state.jpeg
                if not body:
                    self.send_error(503, "waiting for decoded video")
                    return
                kind = "image/jpeg"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
    return Handler


def arguments(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--demo", action="store_true", help="generate and loop reference video IQ")
    source.add_argument("--file", type=Path)
    source.add_argument("--stdin", action="store_true")
    source.add_argument("--e200", action="store_true", help="COORDINATOR ONLY: configure and stream E200 RX")
    p.add_argument("--sample-rate", type=int)
    p.add_argument("--symbol-rate", type=int)
    p.add_argument("--format", choices=("s16", "f32"), default="s16")
    p.add_argument("--scale", type=float, help="FFT full scale: E200 2048, board 512, f32 1")
    p.add_argument("--loop", action="store_true")
    p.add_argument("--unpaced", action="store_true", help="file throughput check, not live playback")
    p.add_argument("--receiver", default=os.environ.get("LEANDVB", str(decode.DEFAULT_RECEIVER)))
    p.add_argument("--flips", type=int, default=200)
    p.add_argument("--tune", type=float, default=0, help="carrier offset from center, Hz")
    p.add_argument("--center", type=float, default=2420000000)
    p.add_argument("--uri", default="ip:192.168.1.10")
    p.add_argument("--bandwidth", type=int, default=13500000)
    p.add_argument("--gain", type=float, default=0)
    p.add_argument("--libiio", type=Path)
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--seconds", type=float, help="stop automatically and print final JSON")
    p.add_argument("--no-open", action="store_true")
    p.add_argument("--ts-out", type=Path, help="save received TS, refuses overwrite")
    p.add_argument("--log", type=Path, help="save receiver diagnostics, refuses overwrite")
    p.add_argument("--telemetry", type=Path, help="tail regular JSONL file; never open serial here")
    a = p.parse_args(argv)
    if a.demo:
        folder = Path("notes/demo-fixture")
        meta_path = folder / "fixture.json"
        if not meta_path.exists():
            make_fixture(folder)
        meta = json.loads(meta_path.read_text())
        a.file, a.format, a.loop = folder/"video.cf32", "f32", True
        a.sample_rate, a.symbol_rate = meta["sample_rate"], meta["symbol_rate"]
    a.scale = a.scale if a.scale is not None else (2048 if a.format == "s16" else 1)
    if not math.isfinite(a.scale) or a.scale <= 0:
        p.error("scale must be finite and positive")
    if not a.sample_rate or not a.symbol_rate or a.symbol_rate <= 0 or a.sample_rate < 2*a.symbol_rate:
        p.error("positive symbol rate and sample rate >=2*symbol rate required")
    if not all(math.isfinite(x) for x in (a.tune, a.center, a.gain)) or a.center <= 0:
        p.error("finite frequency and gain required")
    if a.seconds is not None and (not math.isfinite(a.seconds) or a.seconds <= 0):
        p.error("seconds must be finite and positive")
    if a.e200 and a.format != "s16":
        p.error("E200 emits s16 I/Q")
    if a.file and (not a.file.is_file() or not a.file.stat().st_size):
        p.error("file must be a nonempty regular file")
    if a.telemetry and not a.telemetry.is_file():
        p.error("telemetry must be an existing regular file")
    for path in (a.log, a.ts_out):
        if path and path.exists():
            p.error(f"refusing to overwrite {path}")
    return a


def main():
    args = arguments()
    state = State(args)
    rx = Receiver(args, state)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler(state))
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        rx.start()
        url = f"http://127.0.0.1:{server.server_port}"
        print(url, flush=True)
        if not args.no_open:
            webbrowser.open(url)
        deadline = time.monotonic() + args.seconds if args.seconds else None
        while not rx.stop.wait(.2):
            if deadline and time.monotonic() >= deadline:
                break
        return 1 if state.data["error"] else 0
    except KeyboardInterrupt:
        return 0
    finally:
        report = state.snapshot()
        report.pop("spectrum")
        report["status"] = "stopped" if report["status"] == "running" else report["status"]
        report["healthy"] = False
        rx.close()
        server.shutdown()
        server.server_close()
        report["video_diagnostics"] = list(getattr(rx, "video_log", []))
        print(json.dumps(report, allow_nan=False), flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
