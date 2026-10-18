"""USB sender, container and coordinator gates with fake serial peers. Never opens a port."""
import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_send', ROOT/'examples/usb-send/send.py')
send = importlib.util.module_from_spec(spec); spec.loader.exec_module(send)
spec = importlib.util.spec_from_file_location('usb_check', ROOT/'examples/usb-send/check.py')
check = importlib.util.module_from_spec(spec); spec.loader.exec_module(check)


def rejects(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except (ValueError, OSError, RuntimeError, TimeoutError):
        return
    raise AssertionError('bad input passed')


class Short(io.BytesIO):
    def read(self, n=-1):
        return super().read(min(n, 17))


def test_container():
    data = bytes(range(256))*29
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)/'unpacked'
        with send.container(Short(data), 'hello.bin', 4016) as stream:
            encoded = stream.read()
        assert len(encoded) % 4016 == 0 and encoded.startswith(bytes(8*4016))
        result = send.unpack(Short(encoded), out)
        assert out.read_bytes() == data and result['sha256'] == hashlib.sha256(data).hexdigest()
        rejects(send.unpack, io.BytesIO(encoded), out)  # no overwrite
        out.unlink()
        at = encoded.index(send.MAGIC)+send.HEADER.size+len('hello.bin')
        corrupt = bytearray(encoded); corrupt[at+10] ^= 1
        rejects(send.unpack, io.BytesIO(corrupt), out)
        rejects(send.unpack, io.BytesIO(encoded[:at+len(data)-1]), out)
        assert not out.exists()
        for name in ('../bad', '/bad', 'a\\b', '.', '..', '', 'x:y', 'a\0b', 'x'*256):
            rejects(send.safe_name, name)
        evil = send.HEADER.pack(send.MAGIC, 6, 0, hashlib.sha256(b'').digest())+b'../bad'
        rejects(send.unpack, io.BytesIO(evil), out)
        rejects(send.unpack, io.BytesIO(b'nothing'), out)
        with send.container(io.BytesIO(), 'empty', 4016) as stream:
            send.unpack(stream, out)
        assert out.read_bytes() == b''
    print('container: short reads, acquisition padding, empty/binary files, SHA, truncation, unsafe paths and no overwrite: ok')


def test_pv():
    packet = bytes((0x47, 1, 0, 0x10))+bytes(range(184))
    vector = send.message(packet, 1, 1)
    assert len(vector) == 1332 and struct.unpack('<I', vector[-4:])[0] == 0x51be328d
    assert send.message(b'a', 65536)[4:6] == bytes(2)
    assert [len(p) for p in send.payloads(Short(packet*8), 1)] == [1316, 188]
    assert b''.join(send.payloads(Short(b'abc'*1000), 2)) == b'abc'*1000
    for payload, kind in ((b'a', 0), (b'', 1), (b'', 2), (packet[:-1], 1), (b'X'*188, 1), (b'a'*1317, 2)):
        rejects(send.message, payload, 0, kind)


def test_id_timing():
    # Compare laptop timing with the real station ID generator, not another
    # Python copy. Cover every Morse character, max length and both DAC rates.
    calls = ['N0CALL', 'A'+'0'*22]+['A0'+c for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789']+['A0/A']
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp); source = folder/'cw.c'; exe = folder/'cw'
        source.write_text('''#include "station_id.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char **argv) {
    station_id_t id;
    for(int i=1;i<argc;++i) for(unsigned scale=1;scale<=2;++scale) {
        assert(station_id_init_rate(&id,argv[i],1,scale*STATION_RATE,NULL,NULL));
        printf("%.9f\\n",(double)id.length/(scale*STATION_RATE));
    }
}''')
        station = ROOT/'firmware/components/station_id'
        subprocess.run(['cc', '-std=c11', '-O2', '-I'+str(station/'include'), str(source),
                        str(station/'station_id.c'), '-lm', '-o', str(exe)], check=True)
        actual = list(map(float, subprocess.check_output([str(exe), *calls], text=True).splitlines()))
    for call, a, b in zip(calls, actual[::2], actual[1::2]):
        assert abs(send.id_seconds(call)-a) < 1e-8 and a == b
    assert abs(send.id_seconds('N0CALL')-5.22) < 1e-8 and send.id_seconds('') == 0
    window, timeout, completion = send.stream_timing('dvbpace 4 20', 20, 5.22, False)
    assert window == 23.22 and timeout == 25 and abs(completion-60.44) < 1e-8
    assert send.stream_timing('dvb 2400 4 20 0', 20, 5.22, False) == (18, 25, 50)
    assert send.stream_timing('dvbdump 4 24', 20, 5.22, True) == (float('inf'), 5, 120)
    # A long callsign and the periodic ID in a 600 s radio-off run also fit.
    cw = send.id_seconds(calls[1])
    assert send.stream_timing('dvbpace 4 1', 1, cw, False)[1] > cw
    assert send.stream_timing('dvbpace 4 600', 600, cw, False)[0] == 598+2*cw
    print('ID timing: actual C Morse generator, all characters, long callsign, 40/80 MHz, paced/keyed/dump budgets: ok')


class Peer:
    """Partial writes, idle writes, fragmented reads; ready precedes data."""
    def __init__(self, dump=False, stop_early=False, fail_after=None):
        self.condition = threading.Condition(); self.rx = bytearray(); self.tx = bytearray()
        self.started = False; self.nop = False; self.dump = dump; self.stop_early = stop_early
        self.messages = self.payload_bytes = self.ts_packets = self.crc_chain = 0
        self.iq = b'@{"done":true}\n\xff\0'*11  # JSON-looking binary is never a console record
        self.writes = 0; self.fail_after = fail_after
        self.controls = []

    def __enter__(self): return self
    def __exit__(self, *args): pass

    def finish(self, recovery=False):
        u = dict(messages=self.messages, payload_bytes=self.payload_bytes, nops=0 if recovery else 1, short=0,
                 header=0, crc=0, sync=0, lost=0, back=0, wrong_type=0, partial_bytes=13 if recovery else 0,
                 ended=not recovery, next_seq=(self.messages+len(self.controls)+(not recovery)) & 65535,
                 ts_packets=self.ts_packets, crc_chain=self.crc_chain, us=18000000, mbps=4.0,
                 wire_bytes=1332*(self.messages+(not recovery))+(13 if recovery else 0),
                 fifo_bytes=1332*(self.messages+(not recovery))+(13 if recovery else 0),
                 driver_bytes=0, driver_pending=0)
        self.emit(dict(cmd=self.cmd, usb=u, controls=len(self.controls), control_rejected=0, status="ESP_OK", encoder_failed=False,
                       frames_encoded=20, underruns=1, late_fills=0, late_switches=0,
                       fatal_late=0, min_fill_slack_cycles=1, input_bytes=self.payload_bytes, seconds=20,
                       payload_bytes_per_frame=4016, symbol_rate=1000000, frame_symbols=33282,
                       crc32=zlib.crc32(self.iq), overlap_mismatches=0))
        self.emit(dict(cmd="usbinput", stop=True))
        self.emit(dict(cmd="usbinput", ended=not recovery, **(dict(recovery="restart", error="missing terminal PV NOP") if recovery else {})))
        if not recovery:
            self.emit(dict(cmd=self.cmd, done=True))

    def emit(self, row):
        self.rx.extend(b'@'+json.dumps(row).encode()+b'\n')

    def write(self, data):
        with self.condition:
            if self.started and self.fail_after is not None and self.messages >= self.fail_after:
                self.tx.extend(data[:13]); self.finish(recovery=True)
                raise TimeoutError('USB transport stalled after an uncertain partial write')
            self.writes += 1
            if self.writes % 7 == 0:
                return 0
            n = min(len(data), 53); self.tx.extend(data[:n])
            if not self.started and b'\n' in self.tx:
                command, _, rest = self.tx.partition(b'\n'); self.tx = bytearray(rest)
                assert command.startswith((b'dvbpace ', b'dvbdump '))
                self.cmd = command.split()[0].decode(); self.started = True
                self.emit(dict(cmd='usbinput', ready=True))
                if self.dump:
                    self.emit(dict(cmd='dvbdump', binary_bytes=len(self.iq)))
                    self.rx.extend(self.iq)
            while self.started and len(self.tx) >= 1332:
                wire = bytes(self.tx[:1332]); del self.tx[:1332]
                assert wire[:3] == b'PV\x01' and zlib.crc32(wire[:-4]) == int.from_bytes(wire[-4:], 'little')
                kind = wire[3]; size = int.from_bytes(wire[6:8], 'little')
                assert int.from_bytes(wire[4:6], 'little') == (self.messages+len(self.controls)) & 65535
                if kind == 0:
                    self.nop = True
                    self.finish()
                elif kind == 3:
                    assert size == 1 and 4 <= wire[12] <= 17
                    self.controls.append(wire[12])
                else:
                    self.messages += 1; self.payload_bytes += size
                    if kind == 1:
                        self.ts_packets += size//188; self.crc_chain = zlib.crc32(wire[-4:], self.crc_chain)
                    if self.stop_early and self.messages == 3:
                        self.emit(dict(cmd='usbinput', stop=True))
            self.condition.notify_all()
            return n

    def read(self, n):
        with self.condition:
            if not self.rx:
                self.condition.wait(.005)
            data = bytes(self.rx[:min(n, 31)]); del self.rx[:len(data)]
            return data


def test_id_backpressure():
    class CWPeer(Peer):
        write_timeout = 5
        def __init__(self, before_pause):
            super().__init__(); self.before_pause = before_pause; self.paused = False
        def write(self, data):
            # No wall-clock sleep: a finite-write transport rejects a timeout
            # shorter than the opening CW pause. The native adapter scheduling
            # model tests the actual queue/clock sequence separately.
            if self.started and self.messages >= self.before_pause and not self.paused:
                self.paused = True
                if self.write_timeout < 5.22:
                    with self.condition:
                        self.finish(recovery=True); self.condition.notify_all()
                    raise TimeoutError('write deadline expired during opening CW')
            return super().write(data)
    for before_pause in (0, 11, 27):
        source = bytes(1316*before_pause)  # pause can block EOS too, not just data
        peer = CWPeer(before_pause)
        stats, rows = send.stream(peer, io.BytesIO(source), cmd='dvbpace 4 20', kind=2,
                                  seconds=20, opening_id=5.22)
        assert peer.paused and peer.nop and not stats['errors'] and stats['write_timeout_seconds'] == 25
        if source:
            send.check_run(stats, rows)
    for before_pause in (11, 27):
        peer = CWPeer(before_pause)
        stats, rows = send.stream(peer, io.BytesIO(bytes(1316*30)), cmd='dvbpace 4 20', kind=2,
                                  seconds=20, opening_id=5.22)
        assert peer.paused and peer.nop and not stats['errors'] and stats['source_eof']
        send.check_run(stats, rows)
    # The regression peer actually rejects the previous five-second policy.
    peer = CWPeer(0); send.write_all(peer, b'dvbpace 4 20\n')
    rejects(send.write_all, peer, send.message(b'a', 0, 2), timeout=5)
    print('USB writer: opening-ID backpressure survives on data and terminal NOP; old timeout fails: ok')


def test_stream():
    data = b'hello\n'*1000
    for dump in (False, True):
        peer = Peer(dump=dump); sink = io.BytesIO()
        stats, rows = send.stream(peer, Short(data), cmd=('dvbdump 4 24' if dump else 'dvbpace 4 20'),
                                  kind=2, seconds=20, dump=dump, sink=sink)
        assert stats['source_eof'] and stats['payload_bytes'] == len(data) and peer.nop
        assert peer.messages == stats['messages'] and not peer.tx
        send.check_run(stats, rows, dump, True)
        report = dict(sender=stats, records=rows)
        check.gates(report, saturation=True)
        check.gates(report, paced=True)
        if dump:
            assert sink.getvalue() == peer.iq
        for field in ('crc', 'lost', 'back', 'wrong_type', 'partial_bytes', 'driver_pending', 'driver_bytes'):
            bad = copy.deepcopy(rows); next(r['usb'] for r in bad if 'usb' in r)[field] = 1
            rejects(send.check_run, stats, bad, dump)
        bad = copy.deepcopy(rows); next(r for r in bad if 'usb' in r)['encoder_failed'] = True
        rejects(send.check_run, stats, bad, dump)
        bad = copy.deepcopy(report); next(r['usb'] for r in bad['records'] if 'usb' in r)['mbps'] = 2.999
        rejects(check.gates, bad, saturation=True)
        for mbps in (3.0, 3.226):
            measured = copy.deepcopy(report); next(r['usb'] for r in measured['records'] if 'usb' in r)['mbps'] = mbps
            check.gates(measured, saturation=True)
        if not dump:
            # A high aggregate rate cannot conceal a dead reader after CW.
            bad = copy.deepcopy(report); run = next(r for r in bad['records'] if 'usb' in r)
            run.update(id_samples=208800000, dac_rate=40000000)
            run['usb'].update(last_message_us=23000001,
                              trace=[dict(reads=20000, payload_bytes=0) for _ in range(31)])
            rejects(check.gates, bad, saturation=True)
            rejects(check.gates, bad, paced=True)
            # Do not qualify a short burst as a sustainable 15-second run.
            bad = copy.deepcopy(report); next(r['usb'] for r in bad['records'] if 'usb' in r)['us'] = 14999999
            rejects(check.gates, bad, saturation=True)
            rejects(check.gates, bad, paced=True)
    packet = bytes((0x47, 1, 0, 0x10))+bytes(184)
    stats, rows = send.stream(Peer(), Short(packet*80), cmd='dvbpace 4 20', kind=1, seconds=20)
    send.check_run(stats, rows)
    assert stats['ts_packets'] == 80 and stats['crc_chain'] != 0
    # Source validation fails at a known boundary: terminal NOP still hands back the console.
    stats, rows = send.stream(Peer(), io.BytesIO(b'invalid TS'), cmd='dvbpace 4 20', kind=1, seconds=20)
    assert stats['errors'] and stats['terminal_nop']
    rejects(send.check_run, stats, rows)
    class BrokenSource:
        def read(self, n):
            raise OSError('source disappeared')
    stats, _ = send.stream(Peer(), BrokenSource(), cmd='dvbpace 4 20', kind=2, seconds=20)
    assert stats['terminal_nop'] and stats['errors'] == ['source disappeared']
    peer = Peer(fail_after=0)
    stats, rows = send.stream(peer, io.BytesIO(bytes(4000)), cmd='dvbpace 4 20', kind=2, seconds=20)
    assert not peer.nop and len(peer.tx) == 13
    assert stats['write_uncertain'] and stats['recovery'] == 'restart' and stats['errors']
    assert any('usb' in r for r in rows), 'writer failure lost the final board counters'
    rejects(send.check_run, stats, rows)
    # A blocked stdin/ffmpeg read cannot prevent a timed end-of-run NOP.
    unblock = threading.Event()
    class Blocked:
        def read(self, n):
            unblock.wait(3); return b''
    peer = Peer(); start = time.monotonic()
    stats, _ = send.stream(peer, Blocked(), cmd='dvbpace 4 1', kind=2, seconds=1)
    unblock.set()
    assert peer.nop and time.monotonic()-start < 1
    # Mid-stream stop completes the current message and closes with NOP.
    peer = Peer(stop_early=True)
    stats, _ = send.stream(peer, io.BytesIO(bytes(100000)), cmd='dvbdump 4 24', kind=2, seconds=20, dump=True)
    assert peer.nop and not stats['source_eof'] and stats['messages'] < 77
    print('USB sender: full duplex, short/idle writes, exact IQ framing, TS chain, source errors, blocked pipe and stop handoff: ok')


def test_live_controls():
    with tempfile.TemporaryDirectory() as tmp:
        control = Path(tmp)/'modcod'
        for mixed, target, rate in ((False, 11, '5m'), (True, 13, '10m')):
            control.write_text('4')
            class LivePeer(Peer):
                def write(self, data):
                    n = super().write(data)
                    if self.messages >= 3:
                        control.write_text(str(target))
                    return n
            peer = LivePeer()
            stats, rows = send.stream(peer, io.BytesIO(bytes(30000)), cmd=f'dvbpace 4 {rate} 20', kind=2,
                                      seconds=20, control=control, rate=rate, mixed=mixed)
            assert peer.controls == [4, target] and stats['controls'] == 2
            assert stats['payload_bytes'] == peer.payload_bytes == 30000
            send.check_run(stats, rows)
        for mc in (0, 3, 18, 255):
            rejects(send.message, bytes((mc,)), 0, 3)
        # These preflight rejections must never open a serial port.
        control.write_text('13')
        source = Path(tmp)/'input.bin'; source.write_bytes(b'bytes')
        for extra in (['--rate','5m'], ['--rate','5m','--mixed']):
            with patch.object(send, 'open_port', side_effect=AssertionError('opened a port')), contextlib.redirect_stderr(io.StringIO()):
                try:
                    send.main(['send',str(source),'--control',str(control),'--report',str(Path(tmp)/'report.json'),*extra])
                except SystemExit as e:
                    assert e.code == 1
                else:
                    raise AssertionError('invalid modulation scope accepted')
    print('USB live MODCOD file: same modulation by default, mixed at 10m, data preserved, preflight scope gate: ok')


def test_safety():
    a = argparse.Namespace(modcod=4, rate='1m', dump=None, key=False, seconds=10, frequency=2400, gain=0, frames=24)
    assert send.command(a) == 'dvbpace 4 10'
    a.dump = Path('x.iq'); assert send.command(a) == 'dvbdump 4 24'
    a.dump = None; a.key = True; assert send.command(a) == 'dvb 2400 4 10 0'
    a.seconds = 61; rejects(send.command, a)
    rejects(send.mode, 13, '1m'); rejects(send.mode, 13, '26m667')
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp)/'x'; source.write_bytes(b'x')
        class Link:
            def __enter__(self): return self
            def __exit__(self, *args): pass
        calls = []
        def collect(link, cmd, timeout=10, records=None):
            calls.append(cmd)
            result = [dict(cmd='info', callsign='', firmware='mock')]
            if records is not None: records.extend(result)
            return result
        with patch.object(send, 'open_port', return_value=Link()), patch.object(send, 'collect', side_effect=collect):
            with contextlib.redirect_stderr(io.StringIO()):
                try:
                    send.main(['send', str(source), '--key', '--report', str(Path(tmp)/'key.json')])
                except SystemExit as e:
                    assert e.code == 1
                else:
                    raise AssertionError('keyed without callsign')
        assert calls == ['info']
    print('defaults radio-off; keyed duration, mode validation and stored-callsign gate: ok')


def test_failure_reports():
    # Run the actual CLI twice: the first connection fails mid-write and still
    # saves firmware counters/recovery. The next connection can send normally.
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp); source = folder/'input.bin'; source.write_bytes(bytes(4000))
        peers = [Peer(fail_after=0), Peer()]
        def collect(link, cmd, timeout=10, records=None):
            assert cmd in ('info', 'vcm off', 'spiinput usb gcs', 'spiinput off')
            result = [dict(cmd=cmd.split()[0], firmware='mock', callsign='N0CALL'),
                      dict(cmd=cmd.split()[0], done=True)]
            if records is not None: records.extend(result)
            return result
        for index in range(2):
            report = folder/f'{index}.json'
            with patch.object(send, 'open_port', return_value=peers[index]), \
                    patch.object(send, 'collect', side_effect=collect), contextlib.redirect_stderr(io.StringIO()):
                try:
                    send.main(['send', str(source), '--kind', 'gcs', '--report', str(report)])
                except SystemExit as e:
                    assert index == 0 and e.code == 1
                else:
                    assert index == 1
            result = json.loads(report.read_text())
            assert result['board_counters_available']
            assert bool(result['errors']) == (index == 0)
            assert result['sender']['recovery'] == ('restart' if index == 0 else None)
            assert any('usb' in r for r in result['records'])
        # Even a preflight/open failure must create the requested report.
        report = folder/'open-failure.json'
        with patch.object(send, 'open_port', side_effect=OSError('port unavailable')), contextlib.redirect_stderr(io.StringIO()):
            try:
                send.main(['send', str(source), '--report', str(report)])
            except SystemExit as e:
                assert e.code == 1
        assert json.loads(report.read_text())['errors'] == ['port unavailable']
    class Writer:
        write_timeout = 0
        def write(self, data):
            assert self.write_timeout > 0, 'nonblocking pyserial can spin on EAGAIN'
            return len(data)
    send.write_all(Writer(), b'abc')
    print('CLI failures: finite serial timeout, retained board counters, report on every exit, next session usable: ok')


def test_reenumeration_and_cancel():
    class Reenumerating:
        is_open = True
        opens = closes = 0
        def close(self): self.is_open = False; self.closes += 1
        def open(self):
            self.opens += 1
            if self.opens < 3: raise OSError('USB device re-enumerating')
            self.is_open = True
    link = Reenumerating(); probes = []
    def info(port, command, timeout, records):
        assert command == 'info'
        probes.append(command)
        if len(probes) == 1:
            records.append(dict(cmd='usbinput', recovery='restart'))
            raise OSError('old descriptor disconnected')
        row = dict(cmd='info', firmware='new', callsign='N0CALL')
        records.append(row)
        return [row]
    rows = []
    with patch.object(send, 'collect', side_effect=info), patch.object(send.time, 'sleep'):
        result = send.console_info(link, rows)
    assert result[0]['firmware'] == 'new' and link.opens == 3 and len(probes) == 2
    assert rows[0]['recovery'] == 'restart' and link.rts is False and link.dtr is False

    class BlockingWrite(Peer):
        def __init__(self):
            super().__init__(); self.cancelled = threading.Event(); self.purged = False
        def reset_output_buffer(self): self.purged = True
        def write(self, data):
            if not self.started: return super().write(data)
            with self.condition:
                self.tx.extend(data[:13]); self.finish(recovery=True)
                self.condition.notify_all()
            assert self.cancelled.wait(2), 'writer was not cancelled on recovery'
            raise OSError('write cancelled after board restart')
        def cancel_write(self): self.cancelled.set()
    peer = BlockingWrite()
    stats, rows = send.stream(peer, io.BytesIO(bytes(4000)), cmd='dvbpace 4 20', kind=2, seconds=20)
    assert peer.cancelled.is_set() and peer.purged and stats['output_purged']
    assert stats['recovery'] == 'restart' and stats['errors']
    assert any('usb' in r for r in rows)
    print('recovery: stale descriptors reopened, read-only info retried, blocked writer cancelled and joined: ok')


def test_coordinator():
    for fail_rate in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)/'check'
            calls = []
            def run(folder, name, source, *args, expect_failure=False):
                calls.append(name)
                assert '--key' not in args
                if '--dump' in args:
                    assert args[args.index('--frames')+1] == '24'
                    Path(args[args.index('--dump')+1]).write_bytes(bytes(16))
                elif name == 'saturation':
                    assert args[args.index('--kind')+1] == 'gcs'
                    assert args[args.index('--modcod')+1] == '13' and '--mbps' not in args
                elif name == 'recovery':
                    assert expect_failure and '--abort-after' in args and args[args.index('--seconds')+1] == '3'
                else:
                    assert args[args.index('--kind')+1] == 'ts' and '--mbps' in args
                u = dict(messages=3, payload_bytes=3948, nops=1, next_seq=4, short=0, header=0,
                         crc=0, sync=0, lost=0, back=0, wrong_type=0, partial_bytes=0, ended=True,
                         ts_packets=0, crc_chain=0, us=18000000,
                         mbps=2.999 if fail_rate and name == 'saturation' else 3.226)
                row = dict(usb=u, status='ESP_OK', encoder_failed=False, frames_encoded=20,
                           underruns=1, late_fills=0, late_switches=0, fatal_late=0, min_fill_slack_cycles=1, seconds=20,
                           payload_bytes_per_frame=4016, symbol_rate=1000000, frame_symbols=33282,
                           crc32=0, overlap_mismatches=0)
                stats = dict(errors=[], terminal_nop=True, source_eof=True, messages=3,
                             payload_bytes=3948, next_seq=3, ts_packets=0, crc_chain=0,
                             binary_bytes=0, host_crc32=0)
                if name == 'recovery':
                    stats['recovery'] = 'restart'; u.update(messages=1, partial_bytes=17)
                return dict(sender=stats, records=[row], exit_code=1 if expect_failure else 0)
            def decode(capture, **kwargs):
                assert kwargs['sample_rate'] == 4000000 and kwargs['symbol_rate'] == 1000000
                with send.container(io.BytesIO((out/'input.bin').read_bytes()), 'input.bin', 4016) as stream:
                    Path(kwargs['output']).write_bytes(stream.read())
                return dict(frame_lock=True, lock_at_end=True, status_complete=True, stream='gcs',
                            ldpc_bit_flips=0, bch_corrected_bits=0, bch_failed_frames=0,
                            bbheader_rejected_frames=0), b''
            with patch.object(check, 'run', side_effect=run), patch.object(check.decode, 'decode', side_effect=decode), \
                    patch.object(sys, 'argv', ['check.py', str(out)]), contextlib.redirect_stdout(io.StringIO()):
                try:
                    check.main()
                except SystemExit as e:
                    assert fail_rate and e.code == 1
                else:
                    assert not fail_rate
            summary = json.loads((out/'summary.json').read_text())
            assert len(summary['failures']) == int(fail_rate)
            assert calls == ['file', 'saturation', 'recovery', 'pace']
            assert (out/'input.bin').read_bytes() == (out/'recovered.bin').read_bytes()
    print('coordinator: radio-off commands, actual unpack/hash, throughput gate and continuation after failure: ok')


def test_decode(receiver):
    """Optional independent LeanDVB proof using the actual portable C encoder."""
    import numpy as np
    from scipy.signal import upfirdn
    from test_dvbs2_c import NativeEncoder, U8P
    import decode
    data = bytes(range(256))*8
    with tempfile.TemporaryDirectory() as tmp, NativeEncoder() as enc:
        tmp = Path(tmp)
        with send.container(io.BytesIO(data), 'proof.bin', 4016) as source:
            stream = source.read()
        frames = []
        mode = enc.lib.dvbs2_find_mode(4)
        assert enc.lib.dvbs2_fast_prepare(enc.fast, mode, 1, 15) == 0
        for at in range(0, len(stream), 4016):
            payload = np.frombuffer(stream[at:at+4016], np.uint8)
            out = np.empty(33282, np.uint8)
            assert enc.lib.dvbs2_encode_frame(enc.fast, enc.ws, None,
                payload.ctypes.data_as(U8P), len(payload)*8, out.ctypes.data_as(U8P)) == 0
            frames.append(np.exp(1j*np.pi/4*out))
        # Independent root-raised-cosine pulse, 4 samples/symbol, alpha .35.
        t = np.arange(-24, 25, dtype=float)/4
        alpha = .35
        h = np.empty_like(t)
        for i, x in enumerate(t):
            if x == 0:
                h[i] = 1+alpha*(4/np.pi-1)
            else:
                h[i] = (np.sin(np.pi*x*(1-alpha))+4*alpha*x*np.cos(np.pi*x*(1+alpha)))/(np.pi*x*(1-(4*alpha*x)**2))
        path = tmp/'proof.cf32'
        upfirdn(h, np.concatenate(frames), up=4).astype('<c8').tofile(path)
        report, payload = decode.decode(path, sample_rate=4000000, symbol_rate=1000000,
                                        receiver=receiver, timeout=45)
        assert report['frame_lock'] and report['lock_at_end'] and report['stream'] == 'gcs', report
        assert all(report[k] == 0 for k in ('ldpc_bit_flips', 'bch_corrected_bits', 'bch_failed_frames')), report
        result = send.unpack(io.BytesIO(payload), tmp/'recovered.bin')
        assert (tmp/'recovered.bin').read_bytes() == data
        print('C encoder -> LeanDVB -> unpack: 2048 identical bytes, SHA-256 '+result['sha256'])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receiver', type=Path)
    a = p.parse_args()
    test_container(); test_pv(); test_id_timing(); test_id_backpressure(); test_stream(); test_live_controls(); test_safety(); test_failure_reports(); test_reenumeration_and_cancel(); test_coordinator()
    if a.receiver:
        test_decode(a.receiver)
