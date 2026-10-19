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


def fec_codec():
    # Also allow copying host/filelink.py alongside this sender.
    host = Path(__file__).resolve().parent.parent.parent/'host'
    if host.is_dir():
        sys.path.insert(0, str(host))
    import filelink
    return filelink


def fec_parameters(value):
    try:
        return fec_codec().parameters(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def message(payload, seq, kind=2):
    if kind not in (0, 1, 2, 3) or len(payload) > PAYLOAD:
        raise ValueError('invalid PV type or length')
    if (kind == 0 and payload) or (kind != 0 and not payload):
        raise ValueError('NOP must be empty; data must be nonempty')
    if kind == 1 and (len(payload) % 188 or
                      any(payload[i] != 0x47 for i in range(0, len(payload), 188))):
        raise ValueError('TS needs whole 188-byte packets with 0x47 sync bytes')
    if kind == 3 and (len(payload) != 1 or not 4 <= payload[0] <= 17):
        raise ValueError('control needs one MODCOD byte, 4..17')
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
def container(source, name, frame_bytes, fec=None):
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
        if fec is None:
            stream.write(bytes(8*frame_bytes))  # acquisition may lose leading BBFRAMEs
        stream.write(HEADER.pack(MAGIC, len(encoded), length, digest.digest()) + encoded)
        snapshot.seek(0); shutil.copyfileobj(snapshot, stream)
        # Complete the final BBFRAME, then leave four trailing data frames.
        if fec is None:
            stream.write(bytes((-stream.tell()) % frame_bytes + 4*frame_bytes))
        stream.seek(0)
        if fec is None:
            yield stream
        else:
            with tempfile.TemporaryFile() as coded:
                fec_codec().write_stream(stream, coded, frame_bytes, *fec)
                yield coded


def unpack(source, output=None, *, fec=False, bbf=False):
    # Scan a continuous GCS DATA FIELD stream (or --fd-bbf output with headers
    # stripped). Ignore acquisition/padding bytes; never trust a received path.
    if fec or bbf:
        import io
        raw = source.read()
        if bbf:
            # Loading the codec also makes the host BBHEADER parser available.
            fec_codec()
            from decode import parse_bbframes
            frames = parse_bbframes(raw)
            if any(f['stream_type'] != 1 for f in frames):
                raise ValueError('file unpack requires GCS')
            raw = b''.join(f['payload'] for f in frames)
        source = io.BytesIO(fec_codec().recover(raw) if fec else raw)
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


def sender_stats():
    return dict(messages=0, controls=0, control_log=[], payload_bytes=0, ts_packets=0, crc_chain=0, next_seq=0,
                source_eof=False, errors=[], terminal_nop=False, complete=False,
                recovery=None, write_calls=0, wire_bytes_written=0, short_writes=0,
                zero_writes=0, write_uncertain=False, output_purged=False)


def control_schedule(value):
    """Seconds after the opening ID, e.g. 0:4,3:7,6:11,9:4."""
    try:
        entries = tuple((float(t), int(mc)) for t, mc in
                        (part.split(':') for part in value.split(',')))
        if (len(entries) < 2 or entries[0][0] != 0 or
                any(not math.isfinite(t) or t < 0 or not 4 <= mc <= 17 for t, mc in entries) or
                any(b[0] <= a[0] or b[1] == a[1] for a, b in zip(entries, entries[1:]))):
            raise ValueError()
        return entries
    except ValueError as exc:
        raise argparse.ArgumentTypeError('need increasing seconds:MODCOD entries starting at 0, e.g. 0:4,3:7,6:11,9:4') from exc


def id_seconds(callsign):
    # Same 20 WPM marks/spacing as station_id.c, including acquisition silence.
    # Keep this file standalone; tests compare it with the actual C generator.
    if not callsign:
        return 0
    codes = dict(zip('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/', (
        '.- -... -.-. -.. . ..-. --. .... .. .--- -.- .-.. -- -. --- .--. '
        '--.- .-. ... - ..- ...- .-- -..- -.-- --.. '
        '----- .---- ..--- ...-- ....- ..... -.... --... ---.. ----. -..-.').split()))
    return .06*(11+sum(sum(4 if mark == '-' else 2 for mark in codes[c])+2 for c in callsign))


def stream_timing(cmd, seconds, opening_id, dump):
    if dump:
        return math.inf, 5, 120
    # Radio-off pace adds IDs to payload time; keyed runs include them in the
    # existing wall-time limit. A full output ring backpressures USB during CW.
    repeats = max(0, math.ceil(seconds/(540-opening_id))-1) if opening_id else 0
    paced = cmd.startswith('dvbpace ')
    extra = (1+repeats)*opening_id if paced else 0
    wall = seconds+(2+repeats)*opening_id if paced else seconds
    # Keyed time includes the trailing ID. Stop feeding before that pause,
    # leaving the same two-second drain margin as a radio-off payload run.
    window = max(.1, seconds-2)+extra if paced else max(.1, seconds-opening_id-2)
    return window, max(5, seconds+5, opening_id+5), wall+30


def write_all(link, data, timeout=5, stats=None, cancel=None):
    # pyserial's POSIX write_timeout=0 path can busy-loop on EAGAIN inside
    # Serial.write(), bypassing our deadline and cancellation. Use its bounded
    # select-based writer, and never retry a timeout with an unknown byte count.
    if hasattr(link, 'write_timeout') and link.write_timeout != timeout:
        link.write_timeout = timeout
    offset, deadline = 0, time.monotonic()+timeout
    while offset < len(data):
        if cancel is not None and cancel.is_set():
            raise InterruptedError('USB write cancelled')
        started = time.monotonic()
        if stats is not None:
            stats['last_write'] = dict(bytes=len(data), confirmed=offset, requested=len(data)-offset)
        try:
            n = link.write(data[offset:])
        except Exception:
            if stats is not None:
                stats['write_uncertain'] = True
            raise
        elapsed = time.monotonic()-started
        if n is None or n < 0 or n > len(data)-offset:
            raise OSError('invalid serial write count')
        if stats is not None:
            stats['write_calls'] += 1
            stats['wire_bytes_written'] += n
            stats['short_writes'] += 0 < n < len(data)-offset
            stats['zero_writes'] += n == 0
            stats['max_write_seconds'] = max(stats.get('max_write_seconds', 0), elapsed)
        offset += n
        if stats is not None:
            stats['last_write']['confirmed'] = offset
        if n:
            deadline = time.monotonic()+timeout
        elif time.monotonic() >= deadline:
            raise TimeoutError('USB write stalled; session incomplete')
        else:
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


def collect(link, cmd, timeout=10, records=None):
    write_all(link, cmd.encode()+b'\n')
    records = [] if records is None else records
    parser = Records()
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        for row in parser.feed(link.read(8192)):
            records.append(row)
            if row.get('error'):
                raise RuntimeError(row['error'])
            if row.get('done') and row.get('cmd') == cmd.split()[0]:
                return records
    raise TimeoutError(f'no completion for {cmd}')


def stream(link, source, *, cmd, kind, seconds, dump=False, sink=None, mbps=0,
           stats=None, rows=None, abort_after=None, opening_id=0, control=None, schedule=(), rate='1m', mixed=False, modcod=4):
    stop, ready, done = threading.Event(), threading.Event(), threading.Event()
    cancel = threading.Event()
    chunks = queue.Queue(8)
    stats = sender_stats() if stats is None else stats
    rows = [] if rows is None else rows
    parser = Records(sink)
    if dump:
        opening_id = 0
    send_window, write_timeout, completion_timeout = stream_timing(cmd, seconds, opening_id, dump)
    stats.update(opening_id_seconds=opening_id, write_timeout_seconds=write_timeout,
                 send_window_seconds=None if dump else send_window)

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
        started = None
        try:
            while not ready.wait(.05):
                if done.is_set():
                    return
            started = time.monotonic()
            stats['ready_monotonic'] = started
            # Finish early enough to drain the encoder queue before a timed run
            # ends. Source reads live in a daemon so a stalled pipe cannot block EOS.
            limit = started+send_window
            next_send = started
            last_modcod = None
            schedule_index = 0
            while not stop.is_set() and time.monotonic() < limit:
                if control or schedule:
                    selected = control.read_text().strip() if control else ''
                    if schedule and (schedule_index == 0 or
                            (schedule_index < len(schedule) and
                             time.monotonic()-started >= opening_id+schedule[schedule_index][0])):
                        selected = str(schedule[schedule_index][1])
                        schedule_index += 1
                    if selected:
                        mc = int(selected)
                        if rate not in ('1m', '5m', '10m') or (rate == '1m' and mc >= 12):
                            raise ValueError('VCM supports 1m QPSK and 5m/10m')
                        if mixed and rate != '5m':
                            raise ValueError('mixed VCM needs 5m; 10m exceeds encoder budget')
                        if not mixed and (mc < 12) != (modcod < 12):
                            raise ValueError('MODCOD changes must stay within the starting modulation; use --mixed at 5m')
                        wire = message(bytes((mc,)), stats['next_seq'], 3)
                        if mc != last_modcod:
                            sent_at = time.monotonic()
                            write_all(link, wire, timeout=write_timeout, stats=stats, cancel=cancel)
                            stats.setdefault('control_log', []).append(dict(modcod=mc, seq=stats['next_seq'],
                                seconds=sent_at-started, completed_seconds=time.monotonic()-started,
                                payload_bytes=stats['payload_bytes']))
                            stats['controls'] += 1
                            stats['next_seq'] = (stats['next_seq']+1) & 65535
                            last_modcod = mc
                            print(f'VCM {mc} at {sent_at-started:.3f}s; payload byte {stats["payload_bytes"]}', file=sys.stderr)
                try:
                    data = chunks.get(timeout=.05)
                except queue.Empty:
                    continue
                if data is None:
                    stats['source_eof'] = True; break
                if isinstance(data, Exception):
                    stats['errors'].append(str(data)); break
                wire = message(data, stats['next_seq'], kind)
                if abort_after is not None and stats['messages'] >= abort_after:
                    write_all(link, wire[:17], timeout=write_timeout, stats=stats)
                    raise OSError('injected partial USB message for radio-off recovery check')
                # Opening Morse ID/calibration can hold the encoder queue full.
                write_all(link, wire, timeout=write_timeout, stats=stats, cancel=cancel)
                stats['messages'] += 1; stats['payload_bytes'] += len(data)
                stats['ts_packets'] += len(data)//188 if kind == 1 else 0
                if kind == 1:
                    stats['crc_chain'] = zlib.crc32(wire[-4:], stats['crc_chain'])
                stats['next_seq'] = (stats['next_seq']+1) & 65535
                if mbps:
                    next_send = max(next_send, time.monotonic()-len(data)*8/(mbps*1e6)) + len(data)*8/(mbps*1e6)
                    stop.wait(max(0, min(next_send, limit)-time.monotonic()))
            if schedule and schedule_index < len(schedule):
                stats['errors'].append('source/run ended before complete MODCOD schedule')
            write_all(link, message(b'', stats['next_seq'], 0), timeout=write_timeout, stats=stats, cancel=cancel)
            stats['terminal_nop'] = True
        except Exception as e:
            # A write failure has an uncertain byte count. Never retry or append
            # a NOP at an unknown boundary; close and report the incomplete session.
            stats['errors'].append(str(e))
            if abort_after is None and hasattr(link, 'reset_output_buffer'):
                try:
                    link.reset_output_buffer()
                    stats['output_purged'] = True
                except Exception as purge_error:
                    stats['errors'].append(f'cannot discard pending USB output: {purge_error}')
        finally:
            stats['send_seconds'] = time.monotonic()-started if started is not None else 0
            stats['mbps'] = stats['payload_bytes']*8/max(stats['send_seconds'], 1e-9)/1e6
            done.set()

    producer = threading.Thread(target=produce, daemon=True)
    sender = threading.Thread(target=transmit, daemon=True)
    launched = False
    try:
        write_all(link, cmd.encode()+b'\n')
        producer.start(); sender.start(); launched = True
        deadline = time.monotonic()+completion_timeout
        while time.monotonic() < deadline:
            data = link.read(8192)
            if data and dump:
                deadline = time.monotonic()+120
            for row in parser.feed(data):
                rows.append(row)
                if row.get('cmd') == 'usbinput':
                    if row.get('ready'):
                        ready.set()
                    if row.get('stop'):
                        stop.set()
                    if row.get('recovery'):
                        stats['recovery'] = row['recovery']
                if row.get('done') and row.get('cmd') == cmd.split()[0]:
                    stats['complete'] = True
            # A failed writer must KEEP READING through the timed run. The
            # firmware prints its counters before its bounded recovery/reboot.
            if stats['complete'] or stats['recovery']:
                break
        else:
            stats['errors'].append('no run completion or recovery record')
    except Exception as e:
        stats['errors'].append(str(e))
    finally:
        stop.set(); done.set(); cancel.set()
        if launched:
            if sender.is_alive() and hasattr(link, 'cancel_write'):
                link.cancel_write()
            sender.join(6)
            if sender.is_alive():
                stats['errors'].append('USB writer did not stop after cancellation')
        stats.update(binary_bytes=parser.binary_bytes, host_crc32=parser.crc,
                     binary_remaining=parser.left, console_partial_bytes=len(parser.pending))
    if not ready.is_set():
        stats['errors'].append('firmware did not start USB input')
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
    if r.get('control_rejected', 0) or r.get('controls', 0) > stats.get('controls', 0):
        raise ValueError('MODCOD control rejected or counter mismatch')
    if u.get('ended') and r.get('controls', 0) != stats.get('controls', 0):
        raise ValueError('MODCOD controls were not fully consumed')
    if r.get('status') != 'ESP_OK' or r.get('encoder_failed') or not r.get('frames_encoded'):
        raise ValueError('encoder failed or encoded no data')
    if u.get('timer_errors', 0) or any(u[k] for k in ('short', 'header', 'crc', 'sync', 'lost', 'back', 'wrong_type', 'partial_bytes')):
        raise ValueError(f'USB protocol errors: {u}')
    if {'wire_bytes', 'fifo_bytes', 'driver_bytes'} <= u.keys():
        if u['wire_bytes'] != u['fifo_bytes']+u['driver_bytes']:
            raise ValueError('USB FIFO/driver byte accounting disagrees')
    if u.get('driver_pending', 0):
        raise ValueError('USB driver still holds unread session bytes')
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
    if r.get('encoder_stack_min_free', 256) < 256:
        raise ValueError('encoder stack reserve below 256 bytes')
    capacity = 8*r['payload_bytes_per_frame']*r['symbol_rate']/r['frame_symbols']/1e6
    # Whole-second receive bins omit startup/CW backpressure and the partial
    # final second. Retain the original wall rate and all raw counters too.
    measured_us, measured_bytes, measured_mbps = u['us'], u['payload_bytes'], u['mbps']
    start_second = None
    if not dump and 'trace' in u:
        opening = r.get('id_samples', 0)/r.get('dac_rate', 40000000)
        start_second = math.ceil(opening+.2)  # encoder prefill is bounded at 100 ms
        end_second = min(len(u['trace']), u['last_message_us']//1000000)
        measured_us = max(0, end_second-start_second)*1000000
        measured_bytes = sum(t['payload_bytes'] for t in u['trace'][start_second:end_second])
        measured_mbps = measured_bytes*8/measured_us if measured_us else 0
    return dict(payload_mbps=measured_mbps, measurement_us=measured_us,
                measurement_payload_bytes=measured_bytes, measurement_start_second=start_second,
                wall_payload_mbps=u['mbps'], run_mbps=u['payload_bytes']*8/r['seconds']/1e6 if 'seconds' in r else None,
                mode_mbps=capacity, frames=r['frames_encoded'], dummies=r['underruns'], usb=u)


def open_port(port):
    import serial
    if not port:
        from serial.tools import list_ports
        discovery_deadline = time.monotonic()+10
        while True:
            ports = [p.device for p in list_ports.comports() if p.vid == 0x303a and p.pid == 0x1001]
            if len(ports) == 1:
                port = ports[0]; break
            if ports or time.monotonic() >= discovery_deadline:
                raise ValueError('specify --port (need one native ESP32-S3 USB-Serial-JTAG port)')
            time.sleep(.2)
    link = serial.Serial()
    link.port, link.baudrate, link.timeout, link.write_timeout = port, 115200, .05, 5
    link.rts = link.dtr = True
    deadline = time.monotonic()+10
    while True:
        try:
            link.rts = link.dtr = True
            link.open()
            link.rts = False; link.dtr = link.dtr; link.dtr = False
            return link
        except OSError:
            link.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)  # native USB can re-enumerate after firmware recovery
        except BaseException:
            link.close(); raise


def console_info(link, observations):
    # Reopen a stale descriptor if a recovery reboot overlaps this connection.
    # Retry only info: RF commands are never sent until this probe succeeds.
    deadline = time.monotonic()+15
    last_error = None
    while time.monotonic() < deadline:
        rows = []
        try:
            if not getattr(link, 'is_open', True):
                link.rts = link.dtr = True
                link.open()
                link.rts = False; link.dtr = link.dtr; link.dtr = False
            result = collect(link, 'info', timeout=2, records=rows)
            if any(r.get('cmd') == 'info' and 'firmware' in r for r in result):
                return result
        except OSError as e:
            last_error = e
            link.close()
        except (RuntimeError, TimeoutError) as e:
            last_error = e
        finally:
            observations.extend(rows)
        time.sleep(.2)
    raise TimeoutError(f'console did not answer info after recovery: {last_error}')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    u = sub.add_parser('unpack', help='recover a file from LeanDVB GCS output')
    u.add_argument('file', help='decoded byte stream, or -'); u.add_argument('-o', '--output', type=Path)
    u.add_argument('--fec', action='store_true', help='recover erasure-coded file')
    u.add_argument('--bbf', action='store_true', help='input is LeanDVB --fd-bbf records')
    s = sub.add_parser('send', help='radio off unless --key is supplied')
    s.add_argument('file', nargs='?', default='-')
    s.add_argument('--kind', choices=('file', 'ts', 'gcs'), help='default: .ts -> TS; other files -> container; stdin -> TS')
    s.add_argument('--name', default='stdin.bin', help='container name for stdin')
    s.add_argument('--fec', nargs='?', const='16:4', type=fec_parameters, metavar='K:M',
                   help='file erasure coding; default 16:4; one shard per GCS frame')
    s.add_argument('--port'); s.add_argument('--modcod', type=int, default=4)
    s.add_argument('--rate', choices=('1m', '5m', '10m', '20m', '26m667', '33m333'), default='1m')
    controls = s.add_mutually_exclusive_group()
    controls.add_argument('--control', type=Path, help='live MODCOD text file; enables GCS VCM at 1m/5m/10m')
    controls.add_argument('--schedule', type=control_schedule, help='seconds after opening ID:MODCOD, e.g. 0:4,3:7,6:11,9:4')
    s.add_argument('--mixed', action='store_true', help='allow QPSK/8PSK controls at 5m; requires --control or --schedule')
    s.add_argument('--seconds', type=int, default=10); s.add_argument('--frames', type=int, default=24)
    safety = s.add_mutually_exclusive_group()
    safety.add_argument('--dump', type=Path, help='radio-off IQ dump instead of radio-off pace')
    safety.add_argument('--key', action='store_true', help='explicitly enable RF; requires stored callsign')
    s.add_argument('--frequency', type=int, default=2400, help='integer MHz, 2390..2450, RF only')
    s.add_argument('--gain', type=int, default=0, help='0..17, RF only')
    s.add_argument('--mbps', type=float, default=0, help='optional payload ceiling; 0 uses USB backpressure')
    s.add_argument('--report', type=Path, default=Path('usb-send-report.json'),
                   help='always save diagnostics, including failures (default: usb-send-report.json)')
    s.add_argument('--abort-after', type=int, help=argparse.SUPPRESS)
    a = p.parse_args(argv)
    if a.action == 'send' and ((a.file != '-' and a.report.resolve() == Path(a.file).resolve()) or
                               (a.dump and a.report.resolve() == a.dump.resolve())):
        p.error('report must differ from input and IQ output paths')
    report = dict(command=None, info=[], setup=[], sender=sender_stats(), records=[], errors=[],
                  board_counters_available=False)
    failure = None
    try:
        if a.action == 'unpack':
            with contextlib.ExitStack() as stack:
                source = sys.stdin.buffer if a.file == '-' else stack.enter_context(Path(a.file).open('rb'))
                print(json.dumps(unpack(source, a.output, fec=a.fec, bbf=a.bbf)))
            return
        if (not 1 <= a.seconds <= 600 or not 1 <= a.frames <= 256 or not 2390 <= a.frequency <= 2450 or
                not 0 <= a.gain <= 17 or not math.isfinite(a.mbps) or a.mbps < 0 or
                (a.abort_after is not None and (a.key or a.dump or a.abort_after < 0))):
            raise ValueError('invalid duration, frame count, frequency, gain, rate or recovery check')
        cmd = command(a); report['command'] = cmd
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, indent=2)+'\n')
        frame_bytes, capacity = mode(a.modcod, a.rate)
        kind = a.kind or ('file' if a.fec else 'ts' if a.file == '-' or Path(a.file).suffix.lower() == '.ts' else 'file')
        if a.fec and kind != 'file':
            raise ValueError('--fec requires --kind file')
        live = a.control or a.schedule
        if a.mixed and (not live or a.rate != '5m'):
            raise ValueError('--mixed needs --control/--schedule and --rate 5m (10m exceeds encoder budget)')
        if live and a.fec:
            raise ValueError('--control/--schedule with --fec is not supported')
        if live:
            if kind == 'ts' or a.rate not in ('1m', '5m', '10m'):
                raise ValueError('--control needs file/GCS at 1m, 5m or 10m; TS VCM needs Annex D')
            selected = [int(a.control.read_text().strip())] if a.control else [mc for _, mc in a.schedule]
            if a.schedule and (selected[0] != a.modcod or a.schedule[-1][0] >= a.seconds-2):
                raise ValueError('schedule must start at --modcod and finish before the send window ends')
            for initial in selected:
                message(bytes((initial,)), 0, 3)
                mode(initial, a.rate)
                if not a.mixed and (initial < 12) != (a.modcod < 12):
                    raise ValueError('controls must match the starting modulation; use --mixed at 5m')
            frame_bytes = (58192-80)//8  # acquisition/tail padding for the largest selected frame
        with contextlib.ExitStack() as stack:
            source = sys.stdin.buffer if a.file == '-' else stack.enter_context(Path(a.file).open('rb'))
            if kind == 'file':
                source = stack.enter_context(container(source, a.name if a.file == '-' else Path(a.file).name, frame_bytes, a.fec))
                if a.fec:
                    report['fec'] = dict(k=a.fec[0], m=a.fec[1], frame_bytes=frame_bytes,
                                         frames=source.seek(0, 2)//frame_bytes)
                    source.seek(0)
                    if a.dump and a.frames < report['fec']['frames']:
                        raise ValueError(f"FEC file needs --frames >= {report['fec']['frames']}")
            sink = stack.enter_context(a.dump.open('xb')) if a.dump else None
            link = stack.enter_context(open_port(a.port))
            report['info'] = console_info(link, report['setup'])
            callsign = next((r.get('callsign', '') for r in report['info'] if r.get('cmd') == 'info' and not r.get('done')), '')
            if a.key and not callsign:
                raise ValueError('no stored callsign; set your assigned callsign with the console first')
            if a.schedule and a.key and 2*id_seconds(callsign)+a.schedule[-1][0]+2 >= a.seconds:
                raise ValueError('schedule does not fit between both Morse IDs; increase --seconds')
            collect(link, 'vcm '+('mixed' if a.mixed else 'on' if live else 'off'), records=report['setup'])
            collect(link, 'spiinput usb '+('ts' if kind == 'ts' else 'gcs'), records=report['setup'])
            print(f'{cmd}; {capacity:.3f} Mb/s mode payload; callsign={callsign or "unset"}', file=sys.stderr)
            stats, rows = stream(link, source, cmd=cmd, kind=1 if kind == 'ts' else 2,
                                 seconds=a.seconds, dump=bool(a.dump), sink=sink, mbps=a.mbps,
                                 stats=report['sender'], rows=report['records'], abort_after=a.abort_after,
                                 opening_id=id_seconds(callsign), control=a.control, schedule=a.schedule or (), rate=a.rate, mixed=a.mixed, modcod=a.modcod)
            if stats['complete'] and not stats['recovery']:
                collect(link, 'spiinput off', records=report['setup'])
            summary = check_run(stats, rows, bool(a.dump), kind == 'file')
            report['summary'] = summary
            print(json.dumps(summary), file=sys.stderr)
    except (Exception, KeyboardInterrupt) as e:
        if a.action == 'unpack':
            p.exit(1, f'error: {e}\n')
        failure = str(e) or type(e).__name__
        report['errors'].append(failure)
    finally:
        if a.action == 'send':
            # This runs for preflight, write, read, validation and recovery
            # failures alike. Never lose the board's counters to an exception.
            report['board_counters_available'] = any('usb' in r for r in report['records']+report['setup'])
            try:
                a.report.write_text(json.dumps(report, indent=2)+'\n')
            except OSError as e:
                failure = f'{failure+"; " if failure else ""}cannot save report: {e}'
            print(json.dumps(report), file=sys.stderr)
            print(f'Report: {a.report}', file=sys.stderr)
    if failure:
        p.exit(1, f'error: {failure}\n')


if __name__ == '__main__':
    main()
