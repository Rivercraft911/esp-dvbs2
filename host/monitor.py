#!/usr/bin/env python3
"""Live window on the whole link: cameras, CM5, transmitting S3, radio, receiving S3, viewer.

    python3 host/monitor.py              # listen on UDP 1235, poll pv view on 127.0.0.1:8768
    python3 host/monitor.py --cm5 pigeon@pigeonvision.local:~/esp-dvbs2-ota/live/status.json
    python3 host/monitor.py --demo       # every source synthetic, no hardware

Then open http://127.0.0.1:8770/. Four sources feed it:

- The receiving S3 (`rx ... ts eth`, monitor on) and `host/board.py --monitor` for the
  transmitting S3 send datagrams to UDP port 1235. Each starts with a 12-byte header, little
  endian: "PVMN", version 1, type, a sequence number per type and the low 32 bits of the
  sender's microsecond clock.

      type 1, receiver stats      JSON, five times a second
      type 2, receiver samples    2,048 consecutive int8 I/Q samples of the 16 MS/s capture, ten
                                  times a second, in 4 chunks: u16 snapshot, u8 chunk, u8
                                  chunks, u32 sample rate
      type 3, receiver symbols    up to 640 demodulator output symbols, int8 I/Q at radius 64,
                                  ten times a second: u16 count, u8 bits per symbol, u8 0
      type 4, transmitter stats   JSON, the transmitter's `status` records, once a second

- `--cm5 USER@HOST:PATH`: pv-capture's status file, read once a second over ssh.
- `--viewer URL`: PigeonVision's `pv view` health report, polled once a second.

The page gets everything through server-sent events, with rates derived here from the counters.
`/radio` answers `pv view --radio-monitor-url` with the receiver's lock, MER and frame counts.
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
import subprocess
import threading
import time
import urllib.request

import numpy as np

HEADER = struct.Struct('<4sBBHI')
SAMPLES = struct.Struct('<HBBI')
SYMBOLS = struct.Struct('<HBB')
MAGIC, VERSION = b'PVMN', 1
STATS, SAMPLE, SYMBOL, TX = 1, 2, 3, 4
FFT, BINS, WAVE = 2048, 512, 512
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


class Rate:
    """Per-second rate of a growing counter between two reports."""

    def __init__(self):
        self.last = None

    def __call__(self, value, now: float):
        if value is None:
            return None
        rate = None
        if self.last and value >= self.last[1] and now > self.last[0]:
            rate = (value - self.last[1]) / (now - self.last[0])
        self.last = (now, value)
        return rate


def dig(record: dict, *path):
    for key in path:
        if not isinstance(record, dict):
            return None
        record = record.get(key)
    return record


class Monitor:
    """Parses every source and fans events out to every connected page."""

    def __init__(self):
        self.clients: list[queue.Queue] = []
        self.lock = threading.Lock()
        self.chunks: dict[int, dict[int, bytes]] = {}
        self.stats: dict = {}
        self.latest: dict[str, dict] = {}
        self.rates: dict[str, Rate] = {}
        self.history: list[list] = []    # [time, MER dB, Mb/s out of the receiver]
        self.received = None
        self.datagrams = self.bad = 0
        self.rate = 16_000_000

    def subscribe(self) -> queue.Queue:
        q = queue.Queue(maxsize=256)
        with self.lock:
            self.clients.append(q)
            q.put(('hello', dict(history=self.history[-HISTORY_S * 5:], rate=self.rate, latest=self.latest)))
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.clients.remove(q)

    def emit(self, event: str, data):
        with self.lock:
            for q in self.clients:
                try:
                    q.put_nowait((event, data))
                except queue.Full:       # a stalled page loses updates, never a source
                    pass

    def per_second(self, name: str, value, now: float):
        return self.rates.setdefault(name, Rate())(value, now)

    def publish(self, event: str, record: dict):
        self.latest[event] = record
        self.emit(event, record)

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
        body = data[HEADER.size:]
        if kind == STATS:
            self.received = now
            self.on_rx(json.loads(body), now)
        elif kind == TX:
            self.on_tx(json.loads(body), now)
        elif kind == SAMPLE and len(body) > SAMPLES.size:
            self.received = now
            snap, chunk, chunks, rate = SAMPLES.unpack_from(body)
            parts = self.chunks.setdefault(snap, {})
            parts[chunk] = body[SAMPLES.size:]
            if len(parts) == chunks:
                del self.chunks[snap]
                self.rate = rate
                iq = b''.join(parts[i] for i in range(chunks))
                self.emit('spectrum', dict(rate=rate, db=[round(float(v), 1) for v in spectrum(iq)]))
                self.emit('samples', dict(rate=rate, iq=base64.b64encode(iq[:2 * WAVE]).decode()))
            for old in [s for s in self.chunks if (snap - s) & 0xFFFF > 4 and s != snap]:
                del self.chunks[old]     # a lost chunk never completes its snapshot
        elif kind == SYMBOL and len(body) >= SYMBOLS.size:
            self.received = now
            count, bits, _ = SYMBOLS.unpack_from(body)
            iq = body[SYMBOLS.size:SYMBOLS.size + 2 * count]
            self.emit('symbols', dict(bits=bits, iq=base64.b64encode(iq).decode()))
        else:
            self.bad += 1

    def on_rx(self, s: dict, now: float):
        packets = self.per_second('rx.packets', s.get('packets'), now)
        if packets is not None:
            s['mbps'] = packets * 188 * 8 / 1e6
        self.stats = s
        self.history.append([round(time.time(), 2), s.get('mer_med_db', s.get('mer_db')), s.get('mbps')])
        del self.history[:-HISTORY_S * 5]
        self.publish('rx', s)

    def on_tx(self, s: dict, now: float):
        for key in ('input_bytes', 'payload_bytes', 'spi_bytes'):
            rate = self.per_second('tx.' + key, s.get(key), now)
            if rate is not None:
                s['input_mbps'] = rate * 8 / 1e6
                break
        self.publish('tx', s)

    def on_cm5(self, s: dict, now: float):
        spi = dig(s, 'outputs', 'transport', 'spi') or {}
        out = dict(lifecycle=s.get('lifecycle'), failed=s.get('failed'), messages=spi.get('messages'),
                   ts_packets=spi.get('ts_packets'), ready_timeouts=spi.get('ready_timeouts'),
                   max_ready_wait_ms=spi.get('max_ready_wait_ms'),
                   queue_age_ms=(dig(s, 'outputs', 'transport', 'queue_age_us') or 0) / 1000,
                   dropped=dig(s, 'outputs', 'transport', 'dropped_packets'),
                   video_queue_drops=dig(s, 'outputs', 'transport', 'video_queue_drops'))
        rate = self.per_second('cm5.wire', dig(s, 'outputs', 'transport', 'wire_bytes'), now)
        if rate is not None:
            out['mbps'] = rate * 8 / 1e6
        self.publish('cm5', out)

    def on_viewer(self, s: dict, now: float):
        m = s.get('metrics') or {}
        units = m.get('access_units') or {}
        gaps = m.get('transport_discontinuities') or {}
        out = dict(state=s.get('state'), frames=units, corrupt=m.get('corrupt_packets'),
                   gaps=sum(v for v in gaps.values() if isinstance(v, int)),
                   fps={k: self.per_second('viewer.' + k, v, now) for k, v in units.items()})
        self.publish('viewer', out)

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


def poll_cm5(monitor: Monitor, target: str, key: str | None):
    """One ssh session that prints the status file once a second; reconnects if it drops."""
    host, path = target.split(':', 1)
    opts = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'ServerAliveInterval=5']
    if key:
        opts += ['-i', key, '-o', 'IdentitiesOnly=yes']
    remote = f'while :; do tr -d "\\n" < {path} 2>/dev/null; echo; sleep 1; done'
    while True:
        p = subprocess.Popen(['ssh', *opts, host, remote], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for line in p.stdout:
            try:
                monitor.on_cm5(json.loads(line), time.monotonic())
            except (json.JSONDecodeError, TypeError):
                pass
        p.wait()
        time.sleep(3)


def poll_viewer(monitor: Monitor, url: str):
    while True:
        try:
            with urllib.request.urlopen(url, timeout=0.8) as r:
                monitor.on_viewer(json.load(r), time.monotonic())
        except (OSError, ValueError):
            pass
        time.sleep(1)


class Demo:
    """The whole link made of the same datagrams and records: 8PSK 3/4 short, 5 Msym/s."""

    def __init__(self, seed: int = 2026):
        self.rng = np.random.default_rng(seed)
        self.t0 = time.monotonic()
        self.seq = [0] * 5
        self.frames = self.packets = self.sent = self.dummies = 0
        self.tx_frames = self.tx_dummies = self.tx_bytes = self.messages = self.wire = 0
        self.units = 0
        self.last_stats = 0.0
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
        x = np.fft.ifft(np.fft.fft(noise) * self.shape) * math.sqrt(FFT / self.shape.sum())
        x = 0.16 * x + 0.004 * (self.rng.standard_normal(FFT) + 1j * self.rng.standard_normal(FFT)) + 0.01
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
        sym = 64 * np.exp(1j * (2 * np.pi * k / 8 + 0.01 * math.sin(t)))
        sym = sym + sigma * (self.rng.standard_normal(640) + 1j * self.rng.standard_normal(640))
        s = np.empty(1280, np.int8)
        s[0::2], s[1::2] = np.clip(np.round(sym.real), -127, 127), np.clip(np.round(sym.imag), -127, 127)
        out.append(pack(SYMBOL, self.next_seq(SYMBOL), t_us, SYMBOLS.pack(640, 3, 0) + s.tobytes()))
        if step % 2 == 0:                  # receiver stats at 5 Hz, counting the time that passed
            dt, self.last_stats = t - self.last_stats, t
            data_frames = int(775 * dt * (1 + 0.01 * self.rng.standard_normal()))
            self.frames += data_frames
            self.dummies += int(118 * dt)
            self.packets += int(data_frames * 7.734)
            self.sent += int(data_frames * 7.734 / 7)
            stats = dict(run=1, state='track', freq_mhz=2420.0, rate='5m', modcod=14, short=True,
                         gain=32, frames=self.frames, failed=0, lost=0, relocks=0,
                         dummies=self.dummies, ldpc=0, mer_db=round(mer + 0.2 * self.rng.standard_normal(), 2),
                         mer_med_db=round(mer, 2), cfo_hz=round(-21400 + 30 * math.sin(t / 7)),
                         clock_ppm=round(-10.8 + 0.05 * math.sin(t / 11), 2), packets=self.packets,
                         crc8=0, cc=0, tei=0, eth_sent=self.sent, eth_dropped=0, eth_q_peak=9,
                         eth_mbps_link=10, mon_dropped=0, core0_pct=round(66 + 2 * math.sin(t / 3), 1),
                         capture_drops=0, heap_after_boot=0)
            out.append(pack(STATS, self.next_seq(STATS), t_us, json.dumps(stats).encode()))
        if step % 10 == 0:                 # transmitter status once a second
            self.tx_frames += 775
            self.tx_dummies += 118
            self.tx_bytes += int(9.0e6 / 8)
            tx = dict(cmd='status', elapsed_s=round(t, 1), freq_mhz=2420.0, modcod=14, rate='5m', short=True,
                      frames=self.tx_frames, dummies=self.tx_dummies, underruns=0, late_fills=0,
                      input_bytes=self.tx_bytes, input_messages=self.tx_bytes // 1316, input_rejected=0,
                      encoder_pct=round(41 + math.sin(t / 5), 1), fill_pct=round(44 + 0.5 * math.sin(t / 4), 1))
            out.append(pack(TX, self.next_seq(TX), t_us, json.dumps(tx).encode()))
        return out

    def cm5(self) -> dict:
        self.wire += int(9.11e6 / 8)
        self.messages += int(9.11e6 / 8 / 1332)
        return dict(lifecycle='running', failed=False, outputs=dict(transport=dict(
            wire_bytes=self.wire, queue_age_us=int(180000 + 40000 * self.rng.random()), dropped_packets=0,
            video_queue_drops=0, spi=dict(messages=self.messages, ts_packets=self.messages * 7,
                                          ready_timeouts=0, max_ready_wait_ms=6.2))))

    def viewer(self) -> dict:
        self.units += 30
        return dict(state='streaming', metrics=dict(access_units=dict(A=self.units, B=self.units - 1),
                                                    corrupt_packets=0, transport_discontinuities=dict(A=0, B=0, metadata=0, other=0)))


def run_demo(monitor: Monitor):
    demo, step = Demo(), 0
    while True:
        for d in demo.datagrams(step):
            monitor.datagram(d)
        if step % 10 == 0:
            now = time.monotonic()
            monitor.on_cm5(demo.cm5(), now)
            monitor.on_viewer(demo.viewer(), now)
        step += 1
        time.sleep(0.1)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--port', type=int, default=1235, help='UDP port the S3s send to')
    p.add_argument('--http', type=int, default=8770, help='port of the page on 127.0.0.1')
    p.add_argument('--cm5', help="pv-capture's status file over ssh, USER@HOST:PATH")
    p.add_argument('--ssh-key', help='identity file for --cm5')
    p.add_argument('--viewer', default='http://127.0.0.1:8768/health', help="pv view's health report")
    p.add_argument('--demo', action='store_true', help='every source synthetic')
    a = p.parse_args()
    monitor = Monitor()
    Handler.monitor = monitor
    if a.demo:
        threading.Thread(target=run_demo, args=(monitor,), daemon=True).start()
    else:
        threading.Thread(target=listen, args=(monitor, a.port), daemon=True).start()
        threading.Thread(target=poll_viewer, args=(monitor, a.viewer), daemon=True).start()
        if a.cm5:
            threading.Thread(target=poll_cm5, args=(monitor, a.cm5, a.ssh_key), daemon=True).start()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', a.http), Handler)
    server.daemon_threads = True
    print(f'http://127.0.0.1:{a.http}/  ({"demo" if a.demo else f"UDP {a.port}"})', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
