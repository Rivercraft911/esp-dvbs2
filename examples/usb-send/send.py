#!/usr/bin/env python3
"""Send TS, continuous bytes, or a recoverable file to one ESP32-S3. Only pyserial is required."""
import argparse
import contextlib
import hashlib
import json
import math
from pathlib import Path
import queue
import shutil
import struct
import sys
import tempfile
import threading
import time
import zlib

PAYLOAD = 1316
MAGIC = b'PVFILE1\n'
HEADER = struct.Struct('<8sHQ32s')  # magic, UTF-8 name length, file length, SHA-256
KBCH = (32208, 38688, 43040, 48408, 51648, 53840, 57472, 58192,
        38688, 43040, 48408, 53840, 57472, 58192)


def message(payload, seq, kind=2):
    if kind not in (0, 1, 2) or len(payload) > PAYLOAD:
        raise ValueError('invalid PV type or length')
    if (kind == 0 and payload) or (kind != 0 and not payload):
        raise ValueError('NOP must be empty; data must be nonempty')
    if kind == 1 and (len(payload) % 188 or
                      any(payload[i] != 0x47 for i in range(0, len(payload), 188))):
        raise ValueError('TS needs whole 188-byte packets with 0x47 sync bytes')
    body = struct.pack('<HBBHHI', 0x5650, 1, kind, seq & 65535, len(payload), 0)
    body += payload + bytes(PAYLOAD-len(payload))
    return body + struct.pack('<I', zlib.crc32(body))


def payloads(source, kind):
    pending = bytearray()
    while True:
        part = source.read(PAYLOAD-len(pending))
        if part:
            pending.extend(part)
            if len(pending) < PAYLOAD:
                continue
        elif not pending:
            return
        data = bytes(pending)
        message(data, 0, kind)
        yield data
        pending.clear()
        if not part:
            return


def safe_name(name):
    if (not name or name in ('.', '..') or '/' in name or '\\' in name or
            ':' in name or any(ord(c) < 32 for c in name) or len(name.encode('utf8')) > 255):
        raise ValueError('filename must be a plain basename of at most 255 UTF-8 bytes')
    return name


@contextlib.contextmanager
def container(source, name, frame_bytes):
    # Spooling also supports file containers from stdin, without keeping a large
    # file in RAM. Snapshot first so the length and hash describe the bytes sent.
    with tempfile.TemporaryFile() as snapshot, tempfile.TemporaryFile() as stream:
        digest, length = hashlib.sha256(), 0
        while True:
            part = source.read(65536)
            if not part:
                break
            snapshot.write(part); digest.update(part); length += len(part)
        encoded = safe_name(name).encode('utf8')
        stream.write(bytes(8*frame_bytes))  # receiver acquisition may lose leading BBFRAMEs
        stream.write(HEADER.pack(MAGIC, len(encoded), length, digest.digest()) + encoded)
        snapshot.seek(0); shutil.copyfileobj(snapshot, stream)
        # Complete the final BBFRAME, then leave four trailing data frames.
        stream.write(bytes((-stream.tell()) % frame_bytes + 4*frame_bytes))
        stream.seek(0)
        yield stream


def unpack(source, output=None):
    # Scan a continuous GCS DATA FIELD stream (or --fd-bbf output with headers
    # stripped). Ignore acquisition/padding bytes; never trust a received path.
    window = b''
    while True:
        part = source.read(65536)
        if not part:
            raise ValueError('no complete PVFILE1 header in decoded stream')
        window += part
        at = window.find(MAGIC)
        if at >= 0:
            window = window[at:]
            break
        window = window[-(len(MAGIC)-1):]

    def take(n):
        nonlocal window
        out = window[:n]; window = window[len(out):]
        while len(out) < n:
            part = source.read(n-len(out))
            if not part:
                raise ValueError('truncated file container')
            out += part
        return out

    _, name_len, length, expected = HEADER.unpack(take(HEADER.size))
    if not 1 <= name_len <= 255:
        raise ValueError('invalid filename length')
    name = safe_name(take(name_len).decode('utf8'))
    digest = hashlib.sha256()
    with tempfile.TemporaryFile() as staging:
        left = length
        while left:
            data = take(min(left, 65536)); staging.write(data); digest.update(data); left -= len(data)
        if digest.digest() != expected:
            raise ValueError('SHA-256 mismatch; no output written')
        target = Path(output) if output else Path(name)
        staging.seek(0)
        with target.open('xb') as dest:
            shutil.copyfileobj(staging, dest)
    return dict(file=str(target), name=name, bytes=length, sha256=digest.hexdigest())


def mode(modcod, rate):
    if not 4 <= modcod <= 17 or (rate == '1m' and modcod >= 12) or (rate in ('26m667', '33m333') and modcod >= 12):
        raise ValueError('MODCOD 4..11: QPSK; 12..17: 8PSK; 1m/26m667/33m333 require QPSK')
    frame_bytes = (KBCH[modcod-4]-80)//8
    symbols = 33282 if modcod < 12 else 22194
    symbol_rate = {'1m': 1000000, '5m': 5000000, '10m': 10000000,
                   '20m': 20000000, '26m667': 80000000/3, '33m333': 100000000/3}[rate]
    return frame_bytes, 8*frame_bytes*symbol_rate/symbols/1e6


def command(a):
    mode(a.modcod, a.rate)
    named = '' if a.rate == '1m' else a.rate+' '
    if a.dump:
        return f'dvbdump {a.modcod} {named}{a.frames}'
    if a.key:
        if not 1 <= a.seconds <= 60:
            raise ValueError('keyed duration must be 1..60 seconds')
        return f'dvb {a.frequency} {a.modcod} {named}{a.seconds} {a.gain}'
    return f'dvbpace {a.modcod} {named}{a.seconds}'


def write_all(link, data, timeout=5):
    # Nonblocking serial writes expose short writes, including USB backpressure.
    # Never retry an uncertain timed-out write: its byte count is unknown.
    offset, deadline = 0, time.monotonic()+timeout
    while offset < len(data):
        n = link.write(data[offset:])
        if n is None or n < 0 or n > len(data)-offset:
            raise OSError('invalid serial write count')
        offset += n
        if time.monotonic() >= deadline:
            raise TimeoutError('USB write stalled; session incomplete')
        if not n:
            time.sleep(.001)


class Records:
    """Incremental console/IQ demultiplexer; JSON-looking IQ stays binary."""
    def __init__(self, sink=None):
        self.pending = bytearray(); self.left = 0; self.sink = sink
        self.crc = 0; self.binary_bytes = 0

    def feed(self, data):
        self.pending.extend(data)
        rows = []
        while self.pending:
            if self.left:
                n = min(self.left, len(self.pending))
                part = self.pending[:n]; del self.pending[:n]; self.left -= n
                if self.sink:
                    self.sink.write(part)
                self.crc = zlib.crc32(part, self.crc); self.binary_bytes += n
                continue
            end = self.pending.find(b'\n')
            if end < 0:
                if len(self.pending) > 65536:
                    raise ValueError('oversized console line')
                break
            line = bytes(self.pending[:end]); del self.pending[:end+1]
            if not line.startswith(b'@'):
                continue
            row = json.loads(line[1:]); rows.append(row)
            if 'binary_bytes' in row:
                self.left = int(row['binary_bytes'])
                if self.left < 0:
                    raise ValueError('negative binary length')
        return rows


def collect(link, cmd, timeout=10):
    write_all(link, cmd.encode()+b'\n')
    records, parser = [], Records()
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        for row in parser.feed(link.read(8192)):
            records.append(row)
            if row.get('error'):
                raise RuntimeError(row['error'])
            if row.get('done') and row.get('cmd') == cmd.split()[0]:
                return records
    raise TimeoutError(f'no completion for {cmd}')


def stream(link, source, *, cmd, kind, seconds, dump=False, sink=None, mbps=0):
    stop, ready, done = threading.Event(), threading.Event(), threading.Event()
    chunks = queue.Queue(8)
    stats = dict(messages=0, payload_bytes=0, ts_packets=0, crc_chain=0, next_seq=0,
                 source_eof=False, errors=[], terminal_nop=False)
    rows, parser = [], Records(sink)

    def produce():
        try:
            for data in payloads(source, kind):
                while not stop.is_set():
                    try:
                        chunks.put(data, timeout=.05); break
                    except queue.Full:
                        pass
                if stop.is_set():
                    return
            item = None
        except Exception as e:
            item = e
        while not stop.is_set():
            try:
                chunks.put(item, timeout=.05); return
            except queue.Full:
                pass

    def transmit():
        try:
            while not ready.wait(.05):
                if done.is_set():
                    return
            started = time.monotonic()
            # Finish early enough to drain the encoder queue before a timed run
            # ends. Source reads live in a daemon so a stalled pipe cannot block EOS.
            limit = math.inf if dump else started+max(.1, seconds-2)
            next_send = started
            while not stop.is_set() and time.monotonic() < limit:
                try:
                    data = chunks.get(timeout=.05)
                except queue.Empty:
                    continue
                if data is None:
                    stats['source_eof'] = True; break
                if isinstance(data, Exception):
                    stats['errors'].append(str(data)); break
                wire = message(data, stats['next_seq'], kind)
                # Opening Morse ID/calibration can hold the encoder queue full.
                write_all(link, wire, timeout=seconds+5 if cmd.startswith('dvb ') else 5)
                stats['messages'] += 1; stats['payload_bytes'] += len(data)
                stats['ts_packets'] += len(data)//188 if kind == 1 else 0
                if kind == 1:
                    stats['crc_chain'] = zlib.crc32(wire[-4:], stats['crc_chain'])
                stats['next_seq'] = (stats['next_seq']+1) & 65535
                if mbps:
                    next_send = max(next_send, time.monotonic()-len(data)*8/(mbps*1e6)) + len(data)*8/(mbps*1e6)
                    stop.wait(max(0, min(next_send, limit)-time.monotonic()))
            stats['send_seconds'] = time.monotonic()-started
            stats['mbps'] = stats['payload_bytes']*8/max(stats['send_seconds'], 1e-9)/1e6
            write_all(link, message(b'', stats['next_seq'], 0))
            stats['terminal_nop'] = True
        except Exception as e:
            # A write failure has an uncertain byte count. Never retry or append
            # a NOP at an unknown boundary; close and report the incomplete session.
            stats['errors'].append(str(e))
        finally:
            done.set()

    producer = threading.Thread(target=produce, daemon=True)
    sender = threading.Thread(target=transmit, daemon=True)
    write_all(link, cmd.encode()+b'\n'); producer.start(); sender.start()
    deadline = time.monotonic() + (120 if dump else seconds+30)
    complete = False
    try:
        while time.monotonic() < deadline:
            data = link.read(8192)
            if data:
                deadline = time.monotonic() + (120 if dump else seconds+30)
            for row in parser.feed(data):
                rows.append(row)
                if row.get('cmd') == 'usbinput':
                    if row.get('ready'):
                        ready.set()
                    if row.get('stop'):
                        stop.set()
                if row.get('done') and row.get('cmd') == cmd.split()[0]:
                    complete = True; break
            if complete:
                break
            if done.is_set() and stats['errors'] and not stats['terminal_nop']:
                raise RuntimeError('; '.join(stats['errors']))
        if not complete:
            raise TimeoutError('no run completion (do not send another command into an incomplete session)')
    finally:
        stop.set(); done.set(); sender.join(6)
    if not ready.is_set():
        raise RuntimeError('firmware did not start USB input')
    if sender.is_alive():
        raise TimeoutError('USB sender did not stop')
    stats.update(binary_bytes=parser.binary_bytes, host_crc32=parser.crc)
    for row in rows:
        if row.get('error'):
            stats['errors'].append(row['error'])
    return stats, rows


def check_run(stats, rows, dump=False, require_eof=False):
    if stats['errors'] or not stats['terminal_nop']:
        raise ValueError(f'USB sender failed: {stats}')
    runs = [r for r in rows if 'usb' in r]
    if len(runs) != 1:
        raise ValueError('missing or duplicate USB run result')
    r = runs[0]; u = r['usb']
    if r.get('status') != 'ESP_OK' or r.get('encoder_failed') or not r.get('frames_encoded'):
        raise ValueError('encoder failed or encoded no data')
    if any(u[k] for k in ('short', 'header', 'crc', 'sync', 'lost', 'back', 'wrong_type', 'partial_bytes')):
        raise ValueError(f'USB protocol errors: {u}')
    # A run can end before a live source. Accepted bytes must never exceed sent
    # bytes; finite input must be entirely consumed, including the terminal NOP.
    if not 0 < u['payload_bytes'] <= stats['payload_bytes'] or u['messages'] > stats['messages']:
        raise ValueError('USB sender/receiver counts disagree')
    if require_eof and not stats['source_eof']:
        raise ValueError('file did not finish before the run ended; use more frames/seconds')
    if stats['source_eof'] and (not u['ended'] or u['payload_bytes'] != stats['payload_bytes'] or
                               u['messages'] != stats['messages']):
        raise ValueError('finite source was not fully accepted')
    if u['ended'] and (u['nops'] != 1 or u['next_seq'] != ((stats['next_seq']+1) & 65535)):
        raise ValueError('terminal NOP or final sequence disagrees')
    if u['ts_packets'] > stats['ts_packets'] or (u['messages'] == stats['messages'] and
            (u['ts_packets'] != stats['ts_packets'] or u['crc_chain'] != stats['crc_chain'])):
        raise ValueError('TS packet count or CRC chain disagrees')
    if dump:
        if r['crc32'] != stats['host_crc32'] or r.get('overlap_mismatches'):
            raise ValueError('dump CRC or overlap mismatch')
    elif any(r[k] for k in ('late_fills', 'late_switches', 'fatal_late')) or r['min_fill_slack_cycles'] <= 0:
        raise ValueError('paced output missed a bank deadline')
    capacity = 8*r['payload_bytes_per_frame']*r['symbol_rate']/r['frame_symbols']/1e6
    return dict(payload_mbps=u['mbps'], run_mbps=u['payload_bytes']*8/r['seconds']/1e6 if 'seconds' in r else None,
                mode_mbps=capacity, frames=r['frames_encoded'], dummies=r['underruns'], usb=u)


def open_port(port):
    import serial
    if not port:
        from serial.tools import list_ports
        ports = [p.device for p in list_ports.comports() if p.vid == 0x303a and p.pid == 0x1001]
        if len(ports) != 1:
            raise ValueError('specify --port (need one native ESP32-S3 USB-Serial-JTAG port)')
        port = ports[0]
    link = serial.Serial()
    link.port, link.baudrate, link.timeout, link.write_timeout = port, 115200, .05, 0
    link.rts = link.dtr = True
    try:
        link.open()
        link.rts = False; link.dtr = link.dtr; link.dtr = False
    except BaseException:
        link.close(); raise
    return link


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    u = sub.add_parser('unpack', help='recover a file from LeanDVB GCS output')
    u.add_argument('file', help='decoded byte stream, or -'); u.add_argument('-o', '--output', type=Path)
    s = sub.add_parser('send', help='radio off unless --key is supplied')
    s.add_argument('file', nargs='?', default='-')
    s.add_argument('--kind', choices=('file', 'ts', 'gcs'), help='default: .ts -> TS; other files -> container; stdin -> TS')
    s.add_argument('--name', default='stdin.bin', help='container name for stdin')
    s.add_argument('--port'); s.add_argument('--modcod', type=int, default=4)
    s.add_argument('--rate', choices=('1m', '5m', '10m', '20m', '26m667', '33m333'), default='1m')
    s.add_argument('--seconds', type=int, default=10); s.add_argument('--frames', type=int, default=24)
    safety = s.add_mutually_exclusive_group()
    safety.add_argument('--dump', type=Path, help='radio-off IQ dump instead of radio-off pace')
    safety.add_argument('--key', action='store_true', help='explicitly enable RF; requires stored callsign')
    s.add_argument('--frequency', type=int, default=2400, help='integer MHz, 2390..2450, RF only')
    s.add_argument('--gain', type=int, default=0, help='0..17, RF only')
    s.add_argument('--mbps', type=float, default=0, help='optional payload ceiling; 0 uses USB backpressure')
    s.add_argument('--report', type=Path, help='write sender stats and complete firmware records as JSON')
    a = p.parse_args(argv)
    try:
        if a.action == 'unpack':
            with contextlib.ExitStack() as stack:
                source = sys.stdin.buffer if a.file == '-' else stack.enter_context(Path(a.file).open('rb'))
                print(json.dumps(unpack(source, a.output)))
            return
        if (not 1 <= a.seconds <= 600 or not 1 <= a.frames <= 256 or not 2390 <= a.frequency <= 2450 or
                not 0 <= a.gain <= 17 or not math.isfinite(a.mbps) or a.mbps < 0):
            raise ValueError('invalid duration, frame count, frequency, gain or payload rate')
        cmd = command(a); frame_bytes, capacity = mode(a.modcod, a.rate)
        kind = a.kind or ('ts' if a.file == '-' or Path(a.file).suffix.lower() == '.ts' else 'file')
        with contextlib.ExitStack() as stack:
            source = sys.stdin.buffer if a.file == '-' else stack.enter_context(Path(a.file).open('rb'))
            if kind == 'file':
                source = stack.enter_context(container(source, a.name if a.file == '-' else Path(a.file).name, frame_bytes))
            sink = stack.enter_context(a.dump.open('xb')) if a.dump else None
            link = stack.enter_context(open_port(a.port))
            info = collect(link, 'info')
            callsign = next((r.get('callsign', '') for r in info if r.get('cmd') == 'info' and not r.get('done')), '')
            if a.key and not callsign:
                raise ValueError('no stored callsign; set your assigned callsign with the console first')
            collect(link, 'spiinput usb '+('ts' if kind == 'ts' else 'gcs'))
            print(f'{cmd}; {capacity:.3f} Mb/s mode payload; callsign={callsign or "unset"}', file=sys.stderr)
            stats, rows = stream(link, source, cmd=cmd, kind=1 if kind == 'ts' else 2,
                                 seconds=a.seconds, dump=bool(a.dump), sink=sink, mbps=a.mbps)
            report = dict(command=cmd, info=info, sender=stats, records=rows)
            if a.report:
                a.report.write_text(json.dumps(report, indent=2)+'\n')
            print(json.dumps(report), file=sys.stderr)
            summary = check_run(stats, rows, bool(a.dump), kind == 'file')
            collect(link, 'spiinput off')
            print(json.dumps(summary), file=sys.stderr)
    except (OSError, ValueError, RuntimeError, TimeoutError) as e:
        p.exit(1, f'error: {e}\n')


if __name__ == '__main__':
    main()
