"""File OTA coordinator: erasure accounting, real unpack, RF guards and fake workers. No devices."""
import argparse
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'examples/over-the-air/file_check.py'
spec = importlib.util.spec_from_file_location('file_ota', SCRIPT)
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
import morse  # Import numerical dependencies outside temporary sys.modules patches.


def rejects(fn, *args):
    try:
        fn(*args)
    except (ValueError, RuntimeError):
        return
    raise AssertionError('bad evidence accepted')


def records(payloads):
    result = []
    for payload in payloads:
        header = bytes.fromhex('70000000') + struct.pack('>H', len(payload) * 8) + bytes(3)
        result.append(header + bytes([c.decode.crc8(header)]) + payload)
    return b''.join(result)


def fixture(folder, mc=7):
    original = folder / 'image.png'
    original.write_bytes((ROOT / 'docs/img/fill-history.png').read_bytes())
    expected = c.manifest(original, mc, (16, 4))
    with original.open('rb') as source, c.sender.container(source, original.name, expected['frame_bytes'], (16, 4)) as stream:
        raw = stream.read()
    size = expected['frame_bytes']
    payloads = [raw[i:i+size] for i in range(0, len(raw), size)]
    packets = list(c.filelink.packets_from_stream(raw))
    assert len(packets) == expected['coded_frames']
    assert len(payloads) == expected['frames']
    assert c.filelink.HEADER.unpack_from(packets[0])[1].hex() == expected['identity']
    return original, expected, payloads, packets


def sender_report(a, expected):
    size = expected['frames'] * expected['frame_bytes']
    n = (size + c.sender.PAYLOAD - 1) // c.sender.PAYLOAD
    stats = c.sender.sender_stats()
    stats.update(messages=n, payload_bytes=size, next_seq=n, source_eof=True, terminal_nop=True,
                 complete=True, binary_remaining=0)
    usb = dict(messages=n, payload_bytes=size, nops=1, next_seq=(n+1) & 65535,
               ts_packets=0, crc_chain=0, ended=True, us=1000000, mbps=size*8/1e6,
               **{key: 0 for key in ('short', 'header', 'crc', 'sync', 'lost', 'back', 'wrong_type', 'partial_bytes')})
    row = dict(cmd='dvb', status='ESP_OK', encoder_failed=False, frames_encoded=expected['frames'],
               frames_played=expected['frames'], payload_source='usb', modcod=a.modcod,
               symbol_rate=5000000, frame_symbols=33282, payload_bytes_per_frame=expected['frame_bytes'],
               callsign='N0CALL', freq_mhz=a.mhz, seconds=a.seconds, gain=a.gain, peak_level=128,
               input_pending=0, late_fills=0, late_switches=0, fatal_late=0, min_fill_slack_cycles=1,
               underruns=100, usb=usb)
    return dict(command=f'dvb {a.mhz} {a.modcod} 5m {a.seconds} {a.gain}', errors=[],
                sender=stats, records=[row], fec={k: expected[k] for k in ('k', 'm', 'frame_bytes', 'frames')})


def capture_report(settings):
    tx = dict(lo_powerdown=1, hardwaregain={'voltage0': -89.75},
              dds_raw={f'altvoltage{i}': 0 for i in range(4)})
    return dict(backend='libiio', complete=True, samples=20000000, overflows=None,
                actual=dict(gain_control_mode='manual', **{k: settings[k] for k in ('sample_rate', 'bandwidth', 'frequency', 'gain')}),
                **{key: copy.deepcopy(tx) for key in ('tx_inhibit', 'tx_before_buffer', 'tx_after_capture', 'tx_final')})


def test_recovery():
    for mc in (4, 7):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            original, expected, fields, packets = fixture(folder, mc)
            # A real 237 KB image through the public unpack path, including a
            # source-frame drop in every block and exact original bytes twice.
            clean = folder / 'clean'; clean.mkdir()
            result = c.recovery_cases(records(fields), expected, clean, 1)
            assert result['passed']
            for name, case in result['cases'].items():
                assert Path(case['file']['file']).read_bytes() == original.read_bytes()
                for b in case['blocks']:
                    lost = int(name == 'dropped')
                    assert (b['sent'], b['lost'], b['source_shards_recovered'], b['margin_frames']) == (20, lost, lost, 4-lost)
            # Channel loss and deliberate drops are distinct; duplicates cannot
            # increase margin or undo injection (all copies must be discarded).
            keep = [p for i, p in enumerate(packets) if i % 20 not in (0, 1, 2)]
            keep += keep[:1]
            mixed = folder / 'mixed'; mixed.mkdir()
            result = c.recovery_cases(records(keep[::-1]), expected, mixed, 1)
            assert result['passed'] and result['telemetry']['duplicate_shards'] == 1
            for b in result['cases']['dropped']['blocks']:
                assert (b['channel_lost'], b['injected_dropped'], b['lost'], b['source_shards_recovered'], b['margin_frames']) == (3, 1, 4, 4, 0)
            # M+1 loss: no output from the failed arm; retain the passed normal arm.
            edge = folder / 'edge'; edge.mkdir()
            keep = [p for i, p in enumerate(packets) if i % 20 >= 4]
            result = c.recovery_cases(records(keep), expected, edge, 1)
            assert not result['passed'] and result['cases']['received']['passed']
            assert not result['cases']['dropped']['passed']
            assert all(b['margin_frames'] == -1 for b in result['cases']['dropped']['blocks'])
            assert not (edge / ('dropped-' + expected['name'])).exists()
            # A completely missing tail block still appears in expected counts.
            missing = folder / 'missing'; missing.mkdir()
            result = c.recovery_cases(records(packets[:20]), expected, missing, 1)
            assert not result['passed']
            assert result['cases']['received']['blocks'][-1]['lost'] == 20
            assert result['cases']['received']['blocks'][-1]['margin_frames'] == -16
            # CRC damage becomes an erasure; valid foreign/conflicting data fails.
            damaged = bytearray(packets[0]); damaged[60] ^= 1
            corrupt = folder / 'corrupt'; corrupt.mkdir()
            result = c.recovery_cases(records([bytes(damaged), *packets[1:]]), expected, corrupt, 1)
            assert result['passed'] and result['telemetry']['invalid_packet_frames'] == 1
            changed = bytes(damaged[:-4]) + struct.pack('>I', zlib.crc32(damaged[:-4]))
            rejects(c.received_frames, records([*packets, changed]), expected)
            rejects(c.received_frames, records(packets), dict(expected, identity='00'*32))
            rejects(c.received_frames, records(packets)[:-1], expected)
            # CRC-valid bad content gets through shard counting, fails SHA, and
            # never creates either recovered output (remove redundant parity).
            forged = folder / 'forged'; forged.mkdir()
            forged_packets = [changed, *[p for i, p in enumerate(packets[1:], 1) if i % 20 < 16]]
            result = c.recovery_cases(records(forged_packets), expected, forged, 1)
            assert not result['passed'] and 'SHA-256' in result['cases']['received']['error']
            assert not list(forged.glob('received-*'))
    print('real image: both QPSK modes, source loss, M/M+1, whole-block loss, duplicates, CRC/identity/SHA gates: ok')


def test_guards():
    source = ROOT / 'docs/img/fill-history.png'
    result = subprocess.run([sys.executable, '-S', str(SCRIPT), str(source), '--plan'], capture_output=True)
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['file']['bytes'] == source.stat().st_size and plan['file']['coded_frames'] == 60
    assert plan['rf']['uri'] == 'ip:192.168.1.10' and plan['rf']['sample_rate'] == 10e6
    for extra in ([], ['--key'], ['--key', '--port', 'fake'], ['--closed-box', '--port', 'fake'],
                  ['--key', '--closed-box'], ['--plan', '--uri', 'ip:'], ['--plan', '--seconds', '61'],
                  ['--plan', '--rx-gain', 'nan'], ['--plan', '--drop-per-block', '0'],
                  ['--plan', '--drop-per-block', '5'], ['--plan', '--timeout', 'nan'],
                  ['--plan', '--fec', '1:4', '--drop-per-block', '2'],
                  ['--plan', '--fec', '256:1'], ['--recheck', 'missing', '--key']):
        result = subprocess.run([sys.executable, '-S', str(SCRIPT), str(source), *extra], capture_output=True)
        assert result.returncode == 2 and b'ModuleNotFoundError' not in result.stderr, (extra, result.stderr)
    print('device-free plan, missing authorization/port, invalid settings: refuse before dependencies/devices: ok')


def test_hardware_boundary():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        original, expected, _, _ = fixture(folder)
        a = c.parser().parse_args([str(original), '--key', '--closed-box', '--port', 'fake'])
        a.expected = expected
        settings = c.rf_settings(a)
        report, capture = sender_report(a, expected), capture_report(settings)
        c.validate_sender(report, expected, a, 'N0CALL')
        c.validate_capture(capture, settings)
        for key, value in (('frames_played', expected['frames']-1), ('payload_source', 'xorshift32'),
                           ('late_switches', 1), ('input_pending', 1), ('callsign', 'K1ABC'), ('modcod', 4)):
            bad = copy.deepcopy(report); bad['records'][0][key] = value
            rejects(c.validate_sender, bad, expected, a, 'N0CALL')
        bad = copy.deepcopy(report); bad['sender']['source_eof'] = False
        rejects(c.validate_sender, bad, expected, a, 'N0CALL')
        for key in ('tx_inhibit', 'tx_before_buffer', 'tx_after_capture', 'tx_final'):
            bad = copy.deepcopy(capture); bad[key]['lo_powerdown'] = 0
            rejects(c.validate_capture, bad, settings)
        bad = copy.deepcopy(capture); bad['actual']['sample_rate'] = float('nan')
        rejects(c.validate_capture, bad, settings)
        started = []
        info = [{'cmd': 'info', 'callsign': 'N0CALL'}]
        board = types.SimpleNamespace(reserve_port=lambda port: contextlib.nullcontext(),
                                      run=lambda port, text, **kw: info if text == 'info' and kw['reset'] is False else None)
        class Process:
            def poll(self): return None
            def wait(self, **kw):
                assert (folder / 'stop').exists()
                c.write_json(folder / 'capture.json', capture)
                return 0
        def ready(*args): assert not started
        def send(argv, **kwargs):
            assert '--key' in argv and argv[argv.index('--fec')+1] == '16:4'
            assert kwargs['timeout'] > a.seconds
            started.append(argv)
            c.write_json(folder / 'usb.json', report)
            return types.SimpleNamespace(returncode=0)
        with patch.dict(sys.modules, board=board), patch.object(c.subprocess, 'Popen', return_value=Process()), \
                patch.object(c.subprocess, 'run', side_effect=send), patch.object(c.ota, 'stop') as stop, \
                patch.object(c.ota, 'wait_ready', side_effect=ready), patch.object(c.time, 'sleep'):
            assert c.hardware(a, settings, original, folder)['sent']['coded_frames_sent'] == 60
            assert len(started) == 1 and stop.call_count == 1
            started.clear()
            with patch.object(c.ota, 'wait_ready', side_effect=RuntimeError('TX inhibit failed')):
                rejects(c.hardware, a, settings, original, folder)
            assert not started and stop.call_count == 2
            info[0]['callsign'] = ''
            rejects(c.hardware, a, settings, original, folder)
            assert not started
    print('fake hardware: RX before sender, no key on inhibit/callsign failure, complete-playback/counter/readback gates, cleanup: ok')


def test_coordinator():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        original, expected, fields, _ = fixture(folder)
        out = folder / 'run'
        rx = dict(frame_lock=True, status_complete=True, stream='gcs', modcods=[7], bb_frames=len(fields))
        def hardware(a, settings, source, dest):
            assert source.read_bytes() == original.read_bytes()
            c.write_json(dest / 'info.json', [{'callsign': 'N0CALL'}])
            c.write_json(dest / 'usb.json', sender_report(a, expected))
            c.write_json(dest / 'capture.json', capture_report(settings))
            return dict(sent=dict(coded_frames_sent=60))
        def decode(*args, **kw):
            kw['bb_output'].write_bytes(records(fields))
            return rx, b''.join(fields)
        with patch.object(c, 'hardware', side_effect=hardware), patch.object(c.decode, 'decode', side_effect=decode), \
                patch.object(c.shutil, 'which', return_value='fake'), contextlib.redirect_stdout(io.StringIO()) as stdout:
            assert c.main([str(original), str(out), '--key', '--closed-box', '--port', 'fake']) == 0
            assert 'PASS:' in stdout.getvalue() and 'margin=3 frames' in stdout.getvalue()
            assert json.loads((out / 'report.json').read_text())['passed']
            assert c.main([str(original), str(folder / 'recheck'), '--recheck', str(out)]) == 0, stdout.getvalue()
            assert c.hardware.call_count == 1  # offline recheck must never access devices
            # A decoder without lock cannot be passed by successful file unpack.
            rx['frame_lock'] = False
            assert c.main([str(original), str(folder / 'unlocked'), '--key', '--closed-box', '--port', 'fake']) == 1
            failed = json.loads((folder / 'unlocked/report.json').read_text())
            assert not failed['passed'] and failed['cases']['received']['passed']
            with patch.object(c, 'hardware', side_effect=RuntimeError('capture failed')):
                assert c.main([str(original), str(folder / 'failed'), '--key', '--closed-box', '--port', 'fake']) == 1
            assert 'capture failed' in (folder / 'failed/report.json').read_text()
    print('full coordinator with fake RF/decoder, real unpack/hash, saved offline recheck, failure evidence and exit codes: ok')


def test_iq(receiver):
    """Independent LeanDVB boundary on the real image; generates IQ, no hardware."""
    import numpy as np
    from scipy.signal import upfirdn
    import dvbs2 as d
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        original, expected, fields, _ = fixture(folder)
        a = c.parser().parse_args([str(original), '--receiver', str(receiver)])
        a.expected = expected
        settings = c.rf_settings(a)
        phases = []
        for payload in fields:
            bits = np.unpackbits(np.frombuffer(payload, np.uint8))
            fec = d.fecframe(d.bbframe(bits, '3/4'), '3/4')
            phases.append(d.plframe_phases(fec, '3/4'))
        t, alpha = np.arange(-24, 25, dtype=float)/4, .35
        h = np.array([1+alpha*(4/np.pi-1) if x == 0 else
                      (np.sin(np.pi*x*(1-alpha))+4*alpha*x*np.cos(np.pi*x*(1+alpha)))/
                      (np.pi*x*(1-(4*alpha*x)**2)) for x in t])
        symbols = np.exp(1j*np.pi/4*np.concatenate(phases))
        upfirdn(h, symbols, up=4).astype('<c8').tofile(folder / 'capture.cfile')
        # Same waveform at four samples/symbol; use 20 MS/s for a 5 Msym/s decode.
        settings['sample_rate'] = 20e6
        result = c.analyze(a, settings, folder)
        assert result['passed'], result
        for case in result['cases'].values():
            assert Path(case['file']['file']).read_bytes() == original.read_bytes()
        print(f'OFFLINE MODEL IQ -> independent LeanDVB -> coordinator/unpack: {expected["bytes"]} bytes, '
              f'{expected["coded_frames"]} coded frames, {expected["blocks"]} deliberate source drops, '
              f'SHA-256 {expected["sha256"]}', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receiver', type=Path)
    a = p.parse_args()
    test_recovery(); test_guards(); test_hardware_boundary(); test_coordinator()
    if a.receiver:
        test_iq(a.receiver)
