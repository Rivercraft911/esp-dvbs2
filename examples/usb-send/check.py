#!/usr/bin/env python3
"""Coordinator-only, already flashed board: radio-off file decode and USB rate checks (~2 min)."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'host'))
import decode

spec = importlib.util.spec_from_file_location('usb_send', Path(__file__).with_name('send.py'))
send = importlib.util.module_from_spec(spec)
spec.loader.exec_module(send)

# Measured 3.23 Mb/s on 2026-10-17 at 5 Msym/s with the PSRAM encoder,
# radio off (5059465). Keep a little headroom for host/USB scheduling variation.
SATURATION_FLOOR_MBPS = 3.0


def gates(report, *, saturation=False, paced=False):
    summary = send.check_run(report['sender'], report['records'], bool(report['sender']['binary_bytes']))
    u = summary['usb']
    # These runs stop the sender two seconds early, leaving queue-drain time.
    if not u['ended'] or u['messages'] != report['sender']['messages'] or u['payload_bytes'] != report['sender']['payload_bytes']:
        raise ValueError('sender/board message or byte count differs')
    if (saturation or paced) and summary['measurement_us'] < 15000000:
        raise ValueError(f'less than 15 seconds of steady payload intake: {summary}')
    if saturation and summary['payload_mbps'] < SATURATION_FLOOR_MBPS:
        raise ValueError(f'USB payload below {SATURATION_FLOOR_MBPS:.1f} Mb/s regression floor: {summary}')
    if paced and summary['payload_mbps'] < .98*summary['mode_mbps']:
        raise ValueError(f'USB sender missed mode payload rate: {summary}')
    return summary


def run(out, name, source, *args, expect_failure=False):
    report = out/(name+'.json')
    command = [sys.executable, str(Path(__file__).with_name('send.py')), 'send', str(source),
               '--port', os.environ.get('PORT', '/dev/cu.usbmodem101'), '--report', str(report), *args]
    with (out/(name+'.log')).open('w') as log:
        child = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=110, check=False)
    if not report.exists():
        raise ValueError(f'sender exited {child.returncode} without diagnostics; see {out/(name+".log")}')
    result = json.loads(report.read_text())
    result['exit_code'] = child.returncode
    if child.returncode and not expect_failure:
        board = [r['usb'] for r in result.get('records', []) if 'usb' in r]
        raise ValueError(f'sender failed; report={report}; errors={result.get("errors")}; usb={board}')
    expected = subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, text=True).strip()
    if subprocess.run(['git', 'diff', '--quiet', 'HEAD'], cwd=ROOT).returncode:
        expected += '-dirty'
    versions = [r.get('firmware') for r in result['info'] if r.get('cmd') == 'info' and not r.get('done')]
    if versions != [expected]:
        raise ValueError(f'wrong firmware: expected {expected}, got {versions}')
    return result


def convert_iq(source, target):
    with source.open('rb') as src, target.open('wb') as dst:
        while True:
            data = src.read(65536)
            if not data:
                break
            if len(data) % 4:
                raise ValueError('truncated int16 IQ')
            dst.write(b''.join(struct.pack('<ff', i/512, q/512) for i, q in struct.iter_unpack('<hh', data)))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', nargs='?', type=Path, default=ROOT/'notes'/time.strftime('usb-board-%Y%m%d-%H%M%S'))
    a = p.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    failures, summaries = [], {}
    print(f'Radio off throughout. Evidence: {out}', flush=True)

    def step(name, fn):
        try:
            result = fn(); summaries[name] = result
            print(f'PASS {name}: {json.dumps(result)}', flush=True)
        except Exception as e:
            failures.append(f'{name}: {e}'); print(f'FAIL {name}: {e}', flush=True)

    def file_check():
        original = out/'input.bin'
        original.write_bytes(bytes(range(256))*8)
        iq = out/'file.iq'
        report = run(out, 'file', original, '--dump', str(iq), '--frames', '24')
        gates(report)
        capture = out/'file.cf32'; convert_iq(iq, capture)
        decoded = out/'decoded.gcs'
        rx, _ = decode.decode(capture, sample_rate=4000000, symbol_rate=1000000,
                              receiver=os.environ.get('RX', 'leandvb'), output=decoded,
                              log=out/'leandvb.log', timeout=45)
        (out/'decode.json').write_text(json.dumps(rx, indent=2)+'\n')
        if not rx['frame_lock'] or not rx['lock_at_end'] or not rx['status_complete'] or rx['stream'] != 'gcs':
            raise ValueError(f'LeanDVB did not decode GCS: {rx}')
        for key in ('ldpc_bit_flips', 'bch_corrected_bits', 'bch_failed_frames', 'bbheader_rejected_frames'):
            if rx[key] != 0:
                raise ValueError(f'LeanDVB {key}: {rx[key]}')
        recovered = out/'recovered.bin'
        # Exercise the public subcommand as the coordinator would.
        subprocess.run([sys.executable, str(Path(__file__).with_name('send.py')), 'unpack',
                        str(decoded), '-o', str(recovered)], check=True, timeout=10)
        expected = hashlib.sha256(original.read_bytes()).hexdigest()
        if hashlib.sha256(recovered.read_bytes()).hexdigest() != expected:
            raise ValueError('recovered file hash differs')
        return dict(bytes=original.stat().st_size, sha256=expected, identical=True)

    def saturation():
        source = out/'load.bin'
        with source.open('wb') as f:
            for _ in range(256):
                f.write(bytes(range(256))*256)  # 16 MiB; exceeds a 20-second USB run
        report = run(out, 'saturation', source, '--kind', 'gcs', '--modcod', '13', '--rate', '5m', '--seconds', '20')
        return gates(report, saturation=True)

    def recovery():
        report = run(out, 'recovery', out/'input.bin', '--kind', 'gcs', '--seconds', '3',
                     '--abort-after', '1', expect_failure=True)
        counters = [r['usb'] for r in report['records'] if 'usb' in r]
        if (report['exit_code'] != 1 or report['sender'].get('recovery') != 'restart' or
                not counters or counters[0]['messages'] != 1 or counters[0]['partial_bytes'] != 17):
            raise ValueError(f'partial-write recovery did not retain counters/restart: {report}')
        return dict(expected_failure=True, recovery='restart', usb=counters[0],
                    next_step='fresh info and TS pace must succeed')

    def pace():
        source = out/'pace.ts'
        packet = bytes((0x47, 0x1f, 0xff, 0x10))+bytes(184)
        with source.open('wb') as f:
            for _ in range(4000):
                f.write(packet*7)
        _, capacity = send.mode(4, '1m')
        report = run(out, 'pace', source, '--kind', 'ts', '--seconds', '20', '--mbps', str(capacity))
        return gates(report, paced=True)

    step('file-dump-LeanDVB-unpack', file_check)
    step('sustainable-USB-GCS', saturation)
    step('partial-write-recovery', recovery)
    step('mode-rate-USB-TS-pace', pace)
    (out/'summary.json').write_text(json.dumps(dict(checks=summaries, failures=failures), indent=2)+'\n')
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
