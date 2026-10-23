#!/usr/bin/env python3
"""Live window for the S3 receiver: what its radio hears and how every stage is doing.

    python3 host/monitor.py              # listen for the receiver on UDP 1235
    python3 host/monitor.py --demo       # a synthetic receiver, no hardware

Then open http://127.0.0.1:8770/. The receiver (`rx ... ts eth`, monitor on) sends three kinds
of datagram to UDP port 1235 of the address it streams video to. Each starts with a 12-byte
header, little endian: "PVMN", version 1, type, a sequence number per type and the low 32 bits
of the receiver's microsecond clock.

    type 1, stats    JSON, five times a second: counters from every stage
    type 2, samples  2,048 consecutive int8 I/Q samples of the 16 MS/s capture, ten times a
                     second, in 4 chunks: u16 snapshot, u8 chunk, u8 chunks, u32 sample rate
    type 3, symbols  up to 640 demodulator output symbols, int8 I/Q at radius 64, ten times a
                     second: u16 count, u8 bits per symbol, u8 0

The page gets them through server-sent events: spectra (dBFS, 512 bins), symbols (base64) and
the stats with the payload rate derived here. `/radio` answers PigeonVision's `pv view
--radio-monitor-url` with lock, MER and the frame counters.
"""
from __future__ import annotations

import argparse
import base64
import http.server
import json
import math
import pathlib
import queue
import socket
import struct
import threading
import time

import numpy as np

HEADER = struct.Struct('<4sBBHI')
SAMPLES = struct.Struct('<HBBI')
SYMBOLS = struct.Struct('<HBB')
MAGIC, VERSION = b'PVMN', 1
STATS, SAMPLE, SYMBOL = 1, 2, 3
FFT, BINS = 2048, 512
HISTORY_S = 600
PAGE = pathlib.Path(__file__).with_name('monitor.html')


def blackman_harris(n: int) -> np.ndarray:
    k = 2 * np.pi * np.arange(n) / n
    return 0.35875 - 0.48829 * np.cos(k) + 0.14128 * np.cos(2 * k) - 0.01168 * np.cos(3 * k)


WINDOW = blackman_harris(FFT).astype(np.float32)


def spectrum(iq: bytes, bins: int = BINS) -> np.ndarray:
    """Power per bin in dBFS, lowest frequency first: a full-scale complex tone reads 0 dB."""
    v = np.frombuffer(iq, np.int8).astype(np.float32) / 128
    x = (v[0::2] + 1j * v[1::2])[:FFT]
    power = np.abs(np.fft.fftshift(np.fft.fft(x * WINDOW[:len(x)]))) ** 2 / WINDOW[:len(x)].sum() ** 2
    return 10 * np.log10(power.reshape(bins, -1).mean(1) + 1e-12)


def pack(kind: int, seq: int, t_us: int, body: bytes) -> bytes:
    return HEADER.pack(MAGIC, VERSION, kind, seq & 0xFFFF, t_us & 0xFFFFFFFF) + body


class Monitor:
    """Parses datagrams and fans events out to every connected page."""

    def __init__(self):
        self.clients: list[queue.Queue] = []
        self.lock = threading.Lock()
        self.chunks: dict[int, dict[int, bytes]] = {}
        self.stats: dict = {}
        self.last_packet = None          # (time, packets) for the payload rate
        self.history: list[list] = []    # [time, MER dB, Mb/s]
        self.received = None
        self.datagrams = self.bad = 0
        self.rate = 16_000_000

    def subscribe(self) -> queue.Queue:
        q = queue.Queue(maxsize=256)
        with self.lock:
            self.clients.append(q)
            q.put(('hello', dict(history=self.history[-HISTORY_S * 5:], stats=self.stats,
                                 rate=self.rate)))
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.clients.remove(q)

    def emit(self, event: str, data):
        with self.lock:
            for q in self.clients:
                try:
                    q.put_nowait((event, data))
                except queue.Full:       # a stalled page loses frames, never the receiver
                    pass

    def datagram(self, data: bytes, now: float | None = None):
        now = time.monotonic() if now is None else now
        if len(data) < HEADER.size:
            self.bad += 1
            return
        magic, version, kind, seq, t_us = HEADER.unpack_from(data)
        if magic != MAGIC or version != VERSION:
            self.bad += 1
            return
        self.datagrams += 1
        self.received = now
        body = data[HEADER.size:]
        if kind == STATS:
            self.on_stats(json.loads(body), now)
        elif kind == SAMPLE and len(body) > SAMPLES.size:
            snap, chunk, chunks, rate = SAMPLES.unpack_from(body)
            parts = self.chunks.setdefault(snap, {})
            parts[chunk] = body[SAMPLES.size:]
            if len(parts) == chunks:
                del self.chunks[snap]
                self.rate = rate
                iq = b''.join(parts[i] for i in range(chunks))
                self.emit('spectrum', dict(rate=rate, db=[round(float(v), 1) for v in spectrum(iq)]))
            for old in [s for s in self.chunks if (snap - s) & 0xFFFF > 4 and s != snap]:
                del self.chunks[old]     # a lost chunk never completes its snapshot
        elif kind == SYMBOL and len(body) >= SYMBOLS.size:
            count, bits, _ = SYMBOLS.unpack_from(body)
            iq = body[SYMBOLS.size:SYMBOLS.size + 2 * count]
            self.emit('symbols', dict(bits=bits, iq=base64.b64encode(iq).decode()))
        else:
            self.bad += 1

    def on_stats(self, s: dict, now: float):
        packets = s.get('packets')
        if packets is not None:
            if self.last_packet and packets >= self.last_packet[1] and now > self.last_packet[0]:
                t0, p0 = self.last_packet
                s['mbps'] = (packets - p0) * 188 * 8 / (now - t0) / 1e6
            self.last_packet = (now, packets)
        self.stats = s
        point = [round(time.time(), 2), s.get('mer_med_db', s.get('mer_db')), s.get('mbps')]
        self.history.append(point)
        del self.history[:-HISTORY_S * 5]
        self.emit('stats', s)

    def radio(self) -> dict:
        """PigeonVision's radio monitor format: lock, SNR and cumulative FEC frames/errors."""
        s, fresh = self.stats, self.received is not None and time.monotonic() - self.received < 2
        frames = s.get('frames', 0) + s.get('failed', 0)
        errors = s.get('failed', 0)
        return dict(lock=bool(fresh and s.get('state') == 'track'), snr=s.get('mer_med_db'),
                    fec=dict(frames=frames, errors=errors, fer=errors / frames if frames else None))


class Handler(http.server.BaseHTTPRequestHandler):
    monitor: Monitor

    def log_message(self, *args):
        pass

    def send_body(self, body: bytes, kind: str):
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ('/', '/index.html'):
            self.send_body(PAGE.read_bytes(), 'text/html; charset=utf-8')
        elif self.path == '/radio':
            self.send_body(json.dumps(self.monitor.radio()).encode(), 'application/json')
        elif self.path == '/events':
            self.events()
        else:
            self.send_error(404)

    def events(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        q = self.monitor.subscribe()
        try:
            while True:
                try:
                    event, data = q.get(timeout=1)
                    self.wfile.write(f'event: {event}\ndata: {json.dumps(data)}\n\n'.encode())
                except queue.Empty:
                    self.wfile.write(b': idle\n\n')
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.monitor.unsubscribe(q)


def listen(monitor: Monitor, port: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind(('0.0.0.0', port))
    while True:
        monitor.datagram(sock.recv(2048))


class Demo:
    """A receiver at 8PSK 3/4, 5 Msym/s, 29.8 dB MER, made of the same datagrams."""

    def __init__(self, seed: int = 2026):
        self.rng = np.random.default_rng(seed)
        self.t0 = time.monotonic()
        self.seq = [0, 0, 0, 0]
        self.frames = self.packets = self.sent = self.dummies = 0
        f = np.fft.fftfreq(FFT, 1 / 16e6)
        a, rs = 0.35, 5e6                  # raised-cosine power shape, roll-off 0.35
        lo, hi = (1 - a) * rs / 2, (1 + a) * rs / 2
        h = np.where(np.abs(f) <= lo, 1.0, 0.0)
        edge = (np.abs(f) > lo) & (np.abs(f) <= hi)
        h[edge] = 0.5 * (1 + np.cos(np.pi / (a * rs) * (np.abs(f[edge]) - lo)))
        self.shape = np.sqrt(h)

    def next_seq(self, kind):
        self.seq[kind] = (self.seq[kind] + 1) & 0xFFFF
        return self.seq[kind]

    def datagrams(self, step: int) -> list[bytes]:
        t = time.monotonic() - self.t0
        t_us = int(t * 1e6)
        out = []
        noise = (self.rng.standard_normal(FFT) + 1j * self.rng.standard_normal(FFT)) / math.sqrt(2)
        x = np.fft.ifft(np.fft.fft(noise) * self.shape) * math.sqrt(FFT / self.shape.sum() * 1.0)
        x = 0.16 * x + 0.004 * (self.rng.standard_normal(FFT) + 1j * self.rng.standard_normal(FFT))
        x = x + 0.01                       # a little LO leak at the centre
        iq = np.empty(2 * FFT, np.int8)
        iq[0::2] = np.clip(np.round(x.real * 128), -128, 127)
        iq[1::2] = np.clip(np.round(x.imag * 128), -128, 127)
        snap = self.next_seq(SAMPLE)
        for c in range(4):
            part = iq[c * 1024:(c + 1) * 1024].tobytes()
            out.append(pack(SAMPLE, snap, t_us, SAMPLES.pack(snap, c, 4, 16_000_000) + part))
        k = self.rng.integers(0, 8, 640)
        mer = 29.8 + 0.3 * math.sin(t / 20)
        sigma = 64 / math.sqrt(2 * 10 ** (mer / 10))
        phase = 2 * np.pi * k / 8 + 0.01 * math.sin(t)
        sym = 64 * np.exp(1j * phase) + sigma * (self.rng.standard_normal(640) + 1j * self.rng.standard_normal(640))
        s = np.empty(1280, np.int8)
        s[0::2], s[1::2] = np.clip(np.round(sym.real), -127, 127), np.clip(np.round(sym.imag), -127, 127)
        out.append(pack(SYMBOL, self.next_seq(SYMBOL), t_us, SYMBOLS.pack(640, 3, 0) + s.tobytes()))
        if step % 2 == 0:                  # stats at 5 Hz, counting the time that really passed
            data_frames = int(775 * (t - getattr(self, 'last_stats', t - 0.2)) * (1 + 0.01 * self.rng.standard_normal()))
            self.last_stats = t
            self.frames += data_frames
            self.dummies += 179 - data_frames // 5
            self.packets += int(data_frames * 7.734)
            self.sent += int(data_frames * 7.734 / 7)
            stats = dict(run=1, state='track', freq_mhz=2420.0, rate='5m', modcod=14, short=True,
                         gain=32, frames=self.frames, failed=0, lost=0, relocks=0,
                         dummies=self.dummies, ldpc=0, mer_db=round(mer + 0.2 * self.rng.standard_normal(), 2),
                         mer_med_db=round(mer, 2), cfo_hz=round(-21400 + 30 * math.sin(t / 7)),
                         clock_ppm=round(-10.8 + 0.05 * math.sin(t / 11), 2), packets=self.packets,
                         crc8=0, cc=0, tei=0, eth_sent=self.sent, eth_dropped=0, eth_q_peak=9,
                         mon_dropped=0, core0_pct=round(66 + 2 * math.sin(t / 3), 1),
                         capture_drops=0, heap_after_boot=0)
            out.append(pack(STATS, self.next_seq(STATS), t_us, json.dumps(stats).encode()))
        return out


def run_demo(monitor: Monitor):
    demo, step = Demo(), 0
    while True:
        for d in demo.datagrams(step):
            monitor.datagram(d)
        step += 1
        time.sleep(0.1)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--port', type=int, default=1235, help='UDP port the receiver sends to')
    p.add_argument('--http', type=int, default=8770, help='port of the page on 127.0.0.1')
    p.add_argument('--demo', action='store_true', help='a synthetic receiver instead of UDP')
    a = p.parse_args()
    monitor = Monitor()
    Handler.monitor = monitor
    source = run_demo if a.demo else lambda m: listen(m, a.port)
    threading.Thread(target=source, args=(monitor,), daemon=True).start()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', a.http), Handler)
    server.daemon_threads = True
    print(f'http://127.0.0.1:{a.http}/  ({"demo" if a.demo else f"UDP {a.port}"})', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
