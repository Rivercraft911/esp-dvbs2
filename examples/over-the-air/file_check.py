#!/usr/bin/env python3
"""Closed-box USB/FEC file -> E200 RX -> LeanDVB -> unpack/SHA-256 coordinator.

Only --key --closed-box may access devices. --plan and --recheck are offline.
"""
import argparse
import datetime as dt
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'host'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check as ota
import decode
import filelink

spec = importlib.util.spec_from_file_location('file_ota_sender', ROOT / 'examples/usb-send/send.py')
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def file_hash(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def manifest(path, modcod, fec):
    """Expected identity/counts come from the local source, even if a block vanishes."""
    name = sender.safe_name(path.name).encode('utf8')
    length, digest = path.stat().st_size, file_hash(path)
    header = sender.HEADER.pack(sender.MAGIC, len(name), length, bytes.fromhex(digest)) + name
    identity = hashlib.sha256(header)
    with path.open('rb') as source:
        while part := source.read(65536):
            identity.update(part)
    capacity, _ = sender.mode(modcod, '5m')
    k, m = fec
    size = capacity - filelink.OVERHEAD
    container_bytes = len(header) + length
    blocks = max(1, (container_bytes + k * size - 1) // (k * size))
    if blocks * (k + m) > 2**32:
        raise ValueError('file exceeds FEC sequence space')
    return dict(name=path.name, bytes=length, sha256=digest, identity=identity.hexdigest(),
                container_bytes=container_bytes, modcod=modcod, k=k, m=m, shard_bytes=size,
                frame_bytes=capacity, blocks=blocks, coded_frames=blocks * (k + m),
                guard_frames=12, frames=12 + blocks * (k + m))


def rf_settings(a):
    # Reuse the established streaming worker, validation and fail-closed TX inhibit.
    args = ota.parser().parse_args(['--backend', 'libiio', '--uri', a.uri, '--rate', '5m',
                                   '--modcod', str(a.modcod), '--seconds', str(a.seconds),
                                   '--mhz', str(a.mhz), '--gain', str(a.gain),
                                   '--rx-gain', str(a.rx_gain), '--sample-rate', '10000000'])
    return ota.settings(args)


def send_command(a, source, out):
    return [sys.executable, str(ROOT / 'examples/usb-send/send.py'), 'send', str(source),
            '--port', a.port, '--fec', f'{a.fec[0]}:{a.fec[1]}', '--modcod', str(a.modcod),
            '--rate', '5m', '--seconds', str(a.seconds), '--frequency', str(a.mhz),
            '--gain', str(a.gain), '--key', '--report', str(out / 'usb.json')]


def validate_sender(report, expected, a, call):
    if report.get('errors') or report.get('command') != f'dvb {a.mhz} {a.modcod} 5m {a.seconds} {a.gain}':
        raise ValueError('USB send failed or command differs; see usb.json')
    stats = report['sender']
    sender.check_run(stats, report['records'], require_eof=True)
    if (not stats['complete'] or stats['recovery'] or stats['binary_remaining'] or
            stats['payload_bytes'] != expected['frames'] * expected['frame_bytes'] or
            report.get('fec') != dict(k=expected['k'], m=expected['m'],
                                      frame_bytes=expected['frame_bytes'], frames=expected['frames'])):
        raise ValueError('incomplete finite FEC send or wrong frame counts')
    rows = [r for r in report['records'] if 'usb' in r]
    row = rows[0]
    wanted = dict(cmd='dvb', status='ESP_OK', payload_source='usb', modcod=a.modcod,
                  symbol_rate=5000000, payload_bytes_per_frame=expected['frame_bytes'],
                  callsign=call, freq_mhz=a.mhz, seconds=a.seconds, gain=a.gain, peak_level=128)
    if any(row.get(k) != v for k, v in wanted.items()):
        raise ValueError('board file/mode/callsign differs; see usb.json')
    # frames_played counts DATA frames selected from the ring, excluding dummies.
    # Four drain frames ensure the final coded frame is complete even if the last
    # selected guard overlaps the timed RF boundary.
    if (row.get('frames_encoded') != expected['frames'] or
            row.get('frames_played', 0) < expected['frames'] or row.get('input_pending') != 0):
        raise ValueError('board did not play the complete finite file and drain frames')
    return dict(data_frames_sent=row['frames_played'], coded_frames_sent=expected['coded_frames'],
                guard_frames=expected['guard_frames'], dummy_positions=row['underruns'])


def validate_capture(report, settings):
    if (report.get('backend') != 'libiio' or report.get('complete') is not True or
            report.get('overflows') not in (None, 0) or
            report.get('samples', 0) < settings['sample_rate'] * settings['pre_seconds']):
        raise ValueError('incomplete RX capture; see capture.json and iio.log')
    for key in ('tx_inhibit', 'tx_before_buffer', 'tx_after_capture', 'tx_final'):
        state = report.get(key, {})
        if (state.get('lo_powerdown') != 1 or not state.get('hardwaregain') or
                not state.get('dds_raw') or any(v != -89.75 for v in state['hardwaregain'].values()) or
                any(v != 0 for v in state['dds_raw'].values())):
            raise ValueError(f'missing/failed TX inhibit evidence: {key}')
    actual = report.get('actual', {})
    if actual.get('gain_control_mode') != 'manual':
        raise ValueError('missing manual RX gain readback')
    for key in ('sample_rate', 'bandwidth', 'frequency', 'gain'):
        value = actual.get(key)
        if value is None or not math.isfinite(value) or abs(value - settings[key]) > (.5 if key == 'gain' else 1):
            raise ValueError(f'RX readback differs: {key}')


def hardware(a, settings, source, out):
    import board  # Imported only after explicit hardware authorization and preflight.
    process = None
    with board.reserve_port(a.port), (out / 'iio.log').open('w') as log:
        rows = board.run(a.port, 'info', timeout=10, reset=False)
        write_json(out / 'info.json', rows)
        call = ota.stored_callsign(rows, a.callsign, a.seconds)
        import morse
        # Capacity bound is optimistic about USB; the finite sender/board checks
        # still fail if the actual transfer does not finish in the keyed window.
        required = a.expected['frames'] * 33282 / 5e6
        if required + 2 * morse.duration(call) + 2 > a.seconds:
            raise ValueError('file airtime and both IDs do not fit; increase --seconds')
        try:
            process = subprocess.Popen([a.iio_python, str(Path(__file__).with_name('capture_iio.py')),
                                        str(out / 'settings.json'), '--key'],
                                       stdout=log, stderr=subprocess.STDOUT)
            ota.wait_ready(process, out, settings)
            # Check once more at the boundary; never start the sender after RX exits.
            if process.poll() is not None:
                raise RuntimeError('RX exited before send; board was not keyed')
            print(f'RX ready; sending {a.expected["bytes"]} bytes, {ota.MODES[a.modcod]} at 5 Msym/s', flush=True)
            with (out / 'send.log').open('w') as diagnostics:
                result = subprocess.run(send_command(a, source, out), stdout=diagnostics,
                                        stderr=subprocess.STDOUT, timeout=a.seconds + 90)
            time.sleep(settings['post_seconds'])
            (out / 'stop').touch()
            code = process.wait(timeout=60)
            capture = json.loads((out / 'capture.json').read_text())
            if code:
                raise RuntimeError('RX worker failed; see capture.json and iio.log')
            validate_capture(capture, settings)
            if result.returncode:
                raise RuntimeError('USB sender failed; see usb.json and send.log')
            sent = validate_sender(json.loads((out / 'usb.json').read_text()), a.expected, a, call)
            return dict(callsign=call, sent=sent, capture=capture)
        finally:
            (out / 'stop').touch()
            ota.stop(process)


def received_frames(raw, expected):
    """Count distinct CRC-valid shards; guard fields and bad packets aren't shards."""
    frames = decode.parse_bbframes(raw)
    packets, sequences, invalid, duplicates = {}, [], 0, 0
    for frame in frames:
        if frame['stream_type'] != 1:
            raise ValueError('file reception requires GCS')
        payload = frame['payload']
        valid = list(filelink.packets_from_stream(payload))
        sequence = None
        if valid:
            if valid != [payload]:
                raise ValueError('FEC packet does not occupy exactly one DATA FIELD')
            packet = valid[0]
            _, identity, length, sequence, k, m, size = filelink.HEADER.unpack_from(packet)
            if ((identity.hex(), length, k, m, size) !=
                    (expected['identity'], expected['container_bytes'], expected['k'], expected['m'], expected['shard_bytes']) or
                    len(packet) != expected['frame_bytes'] or not 0 <= sequence < expected['coded_frames']):
                raise ValueError('received foreign identity/parameters/sequence')
            if sequence in packets:
                if packets[sequence] != packet:
                    raise ValueError('conflicting duplicate shard')
                duplicates += 1
            packets[sequence] = packet
        elif any(payload):
            invalid += 1  # CRC-damaged decoded DATA FIELD: an erasure.
        sequences.append(sequence)
    return frames, packets, sequences, dict(decoded_bb_frames=len(frames),
                                            invalid_packet_frames=invalid, duplicate_shards=duplicates)


def recovery_cases(raw, expected, out, drop):
    frames, packets, sequences, telemetry = received_frames(raw, expected)
    k, m = expected['k'], expected['m']
    results = {}
    dropped = set()
    # Drop source shards, so even a perfect RF capture must reconstruct data.
    for block in range(expected['blocks']):
        available = sorted(s for s in packets if s // (k + m) == block and s % (k + m) < k)
        dropped.update(available[:drop])
    for name, losses in (('received', set()), ('dropped', dropped)):
        blocks = []
        for block in range(expected['blocks']):
            sent = set(range(block * (k + m), (block + 1) * (k + m)))
            before = sent & packets.keys()
            kept = before - losses
            missing = sent - kept
            sources_missing = sum(s % (k + m) < k for s in missing)
            blocks.append(dict(block=block, sent=k + m, received=len(kept),
                               channel_lost=len(sent - before), injected_dropped=len(before & losses),
                               lost=len(missing), source_shards_to_recover=sources_missing,
                               source_shards_recovered=0, margin_frames=len(kept) - k,
                               missing_sequences=sorted(missing), dropped_sequences=sorted(before & losses)))
        case = dict(passed=False, blocks=blocks, min_margin_frames=min(b['margin_frames'] for b in blocks))
        results[name] = case
        kept_raw = b''.join(f['header'] + f['payload'] for f, seq in zip(frames, sequences) if seq not in losses)
        (out / (name + '.bbf')).write_bytes(kept_raw)
        target = out / (name + '-' + expected['name'])
        try:
            if any(b['margin_frames'] < 0 for b in blocks):
                raise ValueError('erasure budget exceeded')
            if name == 'dropped' and any(b['injected_dropped'] != drop for b in blocks):
                raise ValueError('not enough received shards for requested injection')
            result = sender.unpack(io.BytesIO(kept_raw), target, fec=True, bbf=True)
            if result['bytes'] != expected['bytes'] or result['sha256'] != expected['sha256'] or file_hash(target) != expected['sha256']:
                raise ValueError('recovered file differs from original SHA-256/length')
            for b in blocks:
                b['source_shards_recovered'] = b['source_shards_to_recover']
            case.update(passed=True, file=result)
        except (OSError, ValueError) as exc:
            case['error'] = str(exc)
    return dict(passed=all(c['passed'] for c in results.values()), telemetry=telemetry, cases=results,
                injection='Discard whole decoded receive BBFRAMEs (all copies of each selected sequence); no second RF transmission')


def analyze(a, settings, out):
    receiver, _ = decode.decode(out / 'capture.cfile', sample_rate=settings['sample_rate'],
                                symbol_rate=settings['symbol_rate'], alpha=.35,
                                receiver=a.receiver, timeout=a.timeout, output=out / 'payload.bin',
                                bb_output=out / 'frames.bbf', log=out / 'leandvb.log')
    write_json(out / 'receiver.json', receiver)
    return audit(a, out, out)


def audit(a, evidence, out):
    receiver = json.loads((evidence / 'receiver.json').read_text())
    result = recovery_cases((evidence / 'frames.bbf').read_bytes(), a.expected, out, a.drop_per_block)
    result['receiver'] = receiver
    # Corrections and failed hypotheses are allowed: this is an erasure link.
    # Require instrumented frame lock, GCS and the requested fixed MODCOD.
    result['receiver_passed'] = bool(receiver.get('frame_lock') and receiver.get('status_complete') and
                                     receiver.get('stream') == 'gcs' and receiver.get('modcods') == [a.modcod] and
                                     receiver.get('bb_frames') == result['telemetry']['decoded_bb_frames'])
    result['passed'] &= result['receiver_passed']
    if not result['receiver_passed']:
        result['error'] = 'LeanDVB lock/mode/diagnostics mismatch; see receiver.json'
    return result


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('file', type=Path, help='real source file (snapshotted before RF)')
    p.add_argument('out', type=Path, nargs='?', help='new evidence directory')
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--plan', action='store_true', help='device-free plan, writes nothing')
    mode.add_argument('--recheck', type=Path, help='audit saved frames/evidence offline, opens no devices')
    mode.add_argument('--key', action='store_true', help='authorize one timed S3 RF send')
    p.add_argument('--closed-box', action='store_true', help='operator confirms both devices/antennas inside closed box')
    p.add_argument('--port', default=os.environ.get('PORT'), help='exact native S3 serial port; no discovery')
    p.add_argument('--uri', default='ip:192.168.1.10')
    p.add_argument('--receiver', default=os.environ.get('RX', ota.DEFAULT_RX))
    p.add_argument('--iio-python', default=ota.RADIOCONDA)
    p.add_argument('--callsign', help='optional stored callsign cross-check; never written')
    p.add_argument('--fec', type=sender.fec_parameters, default=(16, 4), metavar='K:M')
    p.add_argument('--drop-per-block', type=int, default=1, help='additional decoded source frames dropped per block (default 1)')
    p.add_argument('--modcod', type=int, choices=(4, 7), default=7, help='4=QPSK 1/2, 7=QPSK 3/4; fixed 5 Msym/s')
    p.add_argument('--seconds', type=int, default=30)
    p.add_argument('--mhz', type=int, default=2420)
    p.add_argument('--gain', type=int, default=0)
    p.add_argument('--rx-gain', type=float, default=30)
    p.add_argument('--timeout', type=float, default=600, help='offline LeanDVB deadline')
    return p


def main(argv=None):
    p = parser()
    a = p.parse_args(argv)
    try:
        if not (a.plan or a.recheck) and not (a.key and a.closed_box and a.port):
            p.error('hardware requires --key --closed-box --port; use --plan for no devices')
        if not 1 <= a.drop_per_block <= min(a.fec) or not math.isfinite(a.timeout) or a.timeout <= 0:
            raise ValueError('need 1 <= --drop-per-block <= min(K,M) and positive finite --timeout')
        settings = rf_settings(a)
        a.expected = manifest(a.file, a.modcod, a.fec)
        plan = dict(file=a.expected, rf=settings, port=a.port,
                    send_command=send_command(a, Path('OUT/source') / a.file.name, Path('OUT')),
                    drop_per_block=a.drop_per_block,
                    max_disk_bytes=int(ota.capture_bytes(settings) * 1.5 + 100e6 + a.expected['frames'] * a.expected['frame_bytes'] * 4),
                    steps=['Snapshot input; validate stored callsign and both IDs; reserve exact port without reset.',
                           'Start receive-only E200 worker; require TX inhibits/readbacks and one second of IQ before key.',
                           'USB sender --fec --key once; require complete file played and healthy board/USB counters.',
                           'Stop RX after timed send and post-roll; LeanDVB -> frames.bbf.',
                           'Unpack received frames, then drop source frames per block and unpack again; check both SHA-256 values.',
                           'Print per-block sent/lost/recovered/margin, PASS/FAIL; preserve report.json on failure.'])
        if a.plan:
            print(json.dumps(plan, indent=2))
            return 0
        # Fail on missing local dependencies before devices or output directory.
        import numpy  # noqa: F401
        import scipy  # noqa: F401
        if not a.recheck and (not shutil.which(a.receiver) or not shutil.which(a.iio_python)):
            raise ValueError('LeanDVB or capture Python missing; set --receiver / --iio-python')
        out = (a.out or ROOT / 'notes' / dt.datetime.now().strftime('file-ota-%Y%m%d-%H%M%S')).resolve()
        out.mkdir(parents=True, exist_ok=False)
        result = dict(passed=False, file=a.expected, evidence='offline-recheck' if a.recheck else 'rf-capture')
        try:
            write_json(out / 'plan.json', plan)
            write_json(out / 'settings.json', settings)
            write_json(out / 'revision.json', dict(started_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
                       coordinator=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                       dirty=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)),
                       receiver=a.receiver))
            if a.recheck:
                saved = json.loads((a.recheck / 'plan.json').read_text())
                if saved['file'] != a.expected or saved['rf'] != settings:
                    raise ValueError('source/settings differ from saved run; use its original options')
                call = ota.stored_callsign(json.loads((a.recheck / 'info.json').read_text()), a.callsign, a.seconds)
                result['sent'] = validate_sender(json.loads((a.recheck / 'usb.json').read_text()), a.expected, a, call)
                validate_capture(json.loads((a.recheck / 'capture.json').read_text()), settings)
                result.update(audit(a, a.recheck, out))
            else:
                if shutil.disk_usage(out).free < plan['max_disk_bytes']:
                    raise ValueError('insufficient disk for bounded capture')
                source = out / 'source' / a.file.name
                source.parent.mkdir()
                shutil.copyfile(a.file, source)
                if manifest(source, a.modcod, a.fec) != a.expected:
                    raise ValueError('input changed while snapshotting')
                result.update(hardware(a, settings, source, out))
                result.update(analyze(a, settings, out))
        except (Exception, KeyboardInterrupt) as exc:
            result.update(passed=False, error=str(exc) or 'interrupted; S3 retains its timed key window')
        write_json(out / 'report.json', result)
        for name, case in result.get('cases', {}).items():
            print(f'{name}: {"PASS" if case["passed"] else "FAIL"}')
            if case['passed']:
                print(f'  SHA-256 {case["file"]["sha256"]}')
            else:
                print(f'  {case["error"]}')
            for b in case['blocks']:
                print(f'  block {b["block"]}: sent={b["sent"]} lost={b["lost"]} '
                      f'(channel={b["channel_lost"]}, dropped={b["injected_dropped"]}) '
                      f'recovered={b["source_shards_recovered"]} margin={b["margin_frames"]} frames')
        print(f'{"PASS" if result["passed"] else "FAIL"}: {out / "report.json"}' +
              (f' ({result["error"]})' if result.get('error') else ''))
        return 0 if result['passed'] else 1
    except (OSError, ValueError) as exc:
        p.error(str(exc))


if __name__ == '__main__':
    raise SystemExit(main())
