#!/usr/bin/env python3
"""Coordinator-only E200 VCM test for the axis-capable modswap firmware.

--plan, --decode-only and --recheck are offline. Every hardware path requires --key;
coordinator runs it only inside the closed Faraday box. E200 TX stays disabled.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'host'))
import decode
import morse

B = '/Users/river/radioconda/bin'
URI = 'ip:192.168.1.10'
RX = '/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb'
SYMBOL = {'1m': 1e6, '5m': 5e6, '10m': 10e6}


def attr(*args):
    return subprocess.run([f'{B}/iio_attr', '-u', URI, *args], capture_output=True,
                          text=True, check=True).stdout.strip()


def configure(lo, fs, bw, gain):
    attr('-o', '-c', 'ad9361-phy', 'altvoltage1', 'powerdown', '1')
    attr('-o', '-c', 'ad9361-phy', 'voltage0', 'hardwaregain', '-89.75')
    for ch in ('altvoltage0', 'altvoltage1', 'altvoltage2', 'altvoltage3'):
        attr('-o', '-c', 'cf-ad9361-dds-core-lpc', ch, 'raw', '0')
    if attr('-o', '-c', 'ad9361-phy', 'altvoltage1', 'powerdown') != '1':
        raise RuntimeError('E200 TX LO is not powered down')
    attr('-o', '-c', 'ad9361-phy', 'altvoltage0', 'frequency', str(int(lo)))
    attr('-i', '-c', 'ad9361-phy', 'voltage0', 'sampling_frequency', str(int(fs)))
    attr('-i', '-c', 'ad9361-phy', 'voltage0', 'rf_bandwidth', str(int(bw)))
    attr('-i', '-c', 'ad9361-phy', 'voltage0', 'gain_control_mode', 'manual')
    attr('-i', '-c', 'ad9361-phy', 'voltage0', 'hardwaregain', str(gain))
    cfg = dict(rx_lo=attr('-o', '-c', 'ad9361-phy', 'altvoltage0', 'frequency'),
               fs=attr('-i', '-c', 'ad9361-phy', 'voltage0', 'sampling_frequency'),
               bw=attr('-i', '-c', 'ad9361-phy', 'voltage0', 'rf_bandwidth'),
               gain=attr('-i', '-c', 'ad9361-phy', 'voltage0', 'hardwaregain'),
               tx_lo_powerdown=attr('-o', '-c', 'ad9361-phy', 'altvoltage1', 'powerdown'))
    if abs(float(cfg['fs']) - fs) > 1 or abs(float(cfg['rx_lo']) - lo) > 1:
        raise RuntimeError(f'E200 rate/frequency readback differs: {cfg}')
    return cfg


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--pattern', type=lambda s: tuple(map(int, s.split(','))), default=(4, 7, 11))
    p.add_argument('--hold', type=int, default=4, help='frames per pattern entry')
    p.add_argument('--rate', choices=SYMBOL, default='5m')
    p.add_argument('--axes', action='store_true', help='enable the 5m five-level axis renderer')
    p.add_argument('--mhz', type=int, default=2420)
    p.add_argument('--seconds', type=int, default=30)
    p.add_argument('--gain', type=int, default=4, help='S3 gain index, as in the previous E200 tests')
    p.add_argument('--rx-gain', type=float, default=30)
    p.add_argument('--fs', type=float)
    p.add_argument('--bw', type=float)
    p.add_argument('--port', default='/dev/cu.usbmodem1101')
    p.add_argument('--burst', type=int, default=0, help='one contiguous device buffer after the opening ID')
    p.add_argument('--callsign', help='expected stored callsign; required for decode-only without settings.json')
    p.add_argument('--receiver', default=RX)
    p.add_argument('--scale', type=float, default=2048, help='ADC scale; board dumps use 512')
    p.add_argument('--conj', action='store_true')
    p.add_argument('--keep', action='store_true', help='retain IQ on success; failures always retain it')
    p.add_argument('--plan', action='store_true', help='print commands without accessing any device')
    p.add_argument('--decode-only', action='store_true')
    p.add_argument('--recheck', action='store_true', help='recheck saved BBFRAMEs/logs without IQ or devices')
    p.add_argument('--key', action='store_true')
    return p


def settings(a):
    if not 1 <= len(a.pattern) <= 16 or any(mc not in range(4, 18) for mc in a.pattern) or not 1 <= a.hold <= 65535:
        raise ValueError('need 1..16 MODCODs in 4..17 and hold 1..65535')
    mixed = any(mc <= 11 for mc in a.pattern) and any(mc >= 12 for mc in a.pattern)
    if a.axes and a.rate != '5m':
        raise ValueError('axes requires 5m on this build')
    if (mixed and a.rate != '10m' and not (a.rate == '5m' and a.axes)) or (a.rate == '1m' and any(mc >= 12 for mc in a.pattern)):
        raise ValueError('mixed requires 10m or 5m with --axes; 1m supports QPSK only')
    rs = SYMBOL[a.rate]
    fs = a.fs if a.fs is not None else max(2.5e6, 2 * rs)
    bw = a.bw if a.bw is not None else 1.35 * rs * 1.15
    if (not all(math.isfinite(v) for v in (fs, bw, a.rx_gain, a.scale)) or
            fs < 2 * rs or fs > 20e6 or not 1.35 * rs <= bw <= fs or a.scale <= 0 or
            not 1 <= a.seconds <= 60 or not 0 <= a.gain <= 17 or a.burst < 0):
        raise ValueError('invalid rate, bandwidth, duration, gain, scale or burst')
    if a.rate == '10m' and not a.burst and not (a.decode_only or a.recheck):
        raise ValueError('use --burst at 20 MS/s; network streaming is discontinuous')
    board_rate = '' if a.rate == '1m' else a.rate + ' '
    return dict(symbol_rate=rs, sample_rate=fs, bandwidth=bw, pattern=list(a.pattern), hold_frames=a.hold,
                axes=a.axes,
                board_setup=['info', 'spiinput off', 'vcm axes ' + ('on' if a.axes else 'off'),
                             f"vcm {a.hold} " + ' '.join(map(str, a.pattern))],
                board_cleanup=['vcm off', 'vcm axes off'],
                board_command=f'dvb {a.mhz} {a.pattern[0]} {board_rate}{a.seconds} {a.gain} 128',
                burst_samples=a.burst, uri=URI, receiver=str(a.receiver))


def board_argv(a, command):
    return [sys.executable, str(REPO / 'host/board.py'), command, '--port', a.port,
            '--timeout', str(a.seconds + 40), '--out', str(a.out / 'board.jsonl')]


def board_records(result):
    if result.returncode:
        raise RuntimeError(f'board command failed: {result.stdout[-2000:]} {result.stderr[-1000:]}')
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    if not rows or any('error' in row for row in rows):
        raise RuntimeError(f'board returned no result or an error: {rows}')
    return rows


def prepare(a, s):
    for command in s['board_setup']:
        rows = board_records(subprocess.run(board_argv(a, command), capture_output=True, text=True, check=False))
        if command == 'info':
            call = morse.callsign(next(row['callsign'] for row in rows if 'callsign' in row))
            if a.callsign and call != morse.callsign(a.callsign):
                raise RuntimeError('stored callsign differs from --callsign')
            if a.seconds <= 2 * morse.duration(call) + 4:
                raise ValueError('duration leaves too little data after both station IDs')
            s['callsign'] = call
        elif command.startswith('vcm axes '):
            if rows[-1].get('axes') is not a.axes:
                raise RuntimeError(f'axis configuration differs: {rows}')
        elif command.startswith('vcm '):
            row = rows[-1]
            if (row.get('pattern') != list(a.pattern) or row.get('hold_frames') != a.hold or
                    not row.get('enabled') or row.get('axes') is not a.axes):
                raise RuntimeError(f'VCM configuration differs: {row}')
        elif rows[-1].get('input') != 'off':
            raise RuntimeError(f'payload input is still enabled: {rows}')
    return configure(a.mhz * 1e6, s['sample_rate'], s['bandwidth'], a.rx_gain)


def restore(a, s):
    """Try both resets, retaining failures without masking a capture exception."""
    results = []
    for command in s['board_cleanup']:
        try:
            rows = board_records(subprocess.run(board_argv(a, command), capture_output=True,
                                                text=True, timeout=15, check=False))
            row = rows[-1]
            field = 'enabled' if command == 'vcm off' else 'axes'
            if row.get(field) is not False:
                raise RuntimeError(f'{command} was not acknowledged: {rows}')
            results.append(dict(command=command, passed=True))
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            results.append(dict(command=command, passed=False, error=str(exc)))
    s['cleanup'] = results
    (a.out / 'cleanup.json').write_text(json.dumps(results, indent=2) + '\n')
    return all(row['passed'] for row in results)


def capture(a, s, raw):
    count = a.burst or int(s['sample_rate'] * (a.seconds + 4))
    command = [f'{B}/iio_readdev', '-u', URI, '-b', str(a.burst or 1048576),
               '-s', str(count), 'cf-ad9361-lpc', 'voltage0', 'voltage1']
    with raw.open('wb') as sink, (a.out / 'capture.log').open('wb') as caplog:
        if a.burst:
            with (a.out / 'board.log').open('w+') as out, (a.out / 'board.stderr').open('w+') as err:
                board = subprocess.Popen(board_argv(a, s['board_command']), stdout=out, stderr=err, text=True)
                # Opening ID plus Wi-Fi/startup margin, matching the original helper.
                try:
                    time.sleep(morse.duration(s['callsign']) + 2.28)
                    if board.poll() is not None:
                        raise RuntimeError('board stopped before burst capture; see board.log')
                    result = subprocess.run(command, stdout=sink, stderr=caplog, timeout=30, check=False)
                finally:
                    # Collect the finite keyed run even if capture fails.
                    code = board.wait(timeout=a.seconds + 40)
                out.seek(0); err.seek(0)
                rows = board_records(subprocess.CompletedProcess(board.args, code, out.read(), err.read()))
        else:
            receiver = subprocess.Popen(command, stdout=sink, stderr=caplog)
            try:
                time.sleep(2)
                if receiver.poll() is not None:
                    raise RuntimeError('capture stopped before keying; see capture.log')
                result_board = subprocess.run(board_argv(a, s['board_command']), capture_output=True,
                                              text=True, timeout=a.seconds + 45, check=False)
                (a.out / 'board.log').write_text(result_board.stdout + result_board.stderr)
                rows = board_records(result_board)
                result = subprocess.CompletedProcess(command, receiver.wait(timeout=a.seconds + 15))
            finally:
                if receiver.poll() is None:
                    receiver.terminate()
                    receiver.wait(timeout=10)
    if result.returncode or raw.stat().st_size != count * 4:
        raise RuntimeError(f'incomplete capture: {raw.stat().st_size}/{count*4} bytes; see capture.log')
    row = next(row for row in rows if row.get('cmd') == 'dvb' and 'status' in row)
    # Always decode a complete capture. Data integrity and encoder starvation
    # get independent verdicts, rather than losing evidence behind one error.
    s['board_result'] = row
    s['frames_encoded'] = row['frames_encoded']


def board_checks(s):
    row = s.get('board_result', {})
    cleanup = s.get('cleanup', [])
    checks = dict(board_record_present=bool(row),
                  board_vcm_valid=(row.get('status') == 'ESP_OK' and row.get('vcm') is True and
                                   row.get('dac_rate') == 40000000 and row.get('payload_source') == 'xorshift32' and
                                   row.get('callsign') == s['callsign'] and row.get('encoder_failed') is False),
                  board_no_dummies=row.get('underruns') == 0,
                  board_axes_valid=(row.get('vcm_axes') is s['axes'] and
                                    (not s['axes'] or row.get('fill_kernel') == 'pie-axes5')),
                  board_mode_valid=(row.get('symbol_rate') == s['symbol_rate'] and
                                    row.get('modcod') == s['pattern'][0]),
                  board_cleanup_valid=([r.get('command') for r in cleanup] == s['board_cleanup'] and
                                       all(r.get('passed') is True for r in cleanup)))
    checks.update({f'board_{k}_zero': row.get(k) == 0 for k in ('fatal_late', 'late_fills', 'late_switches')})
    return checks


def evaluate(a, s, report, frames):
    if not s.get('cleanup') and (a.out / 'cleanup.json').exists():
        s['cleanup'] = json.loads((a.out / 'cleanup.json').read_text())
    if not s.get('board_result') and (a.out / 'board.jsonl').exists():
        rows = [json.loads(line) for line in (a.out / 'board.jsonl').read_text().splitlines()]
        matches = [row for row in rows if row.get('cmd') == 'dvb' and 'status' in row]
        if matches:
            s['board_result'] = matches[-1]
            s['frames_encoded'] = matches[-1]['frames_encoded']
    upper = s.get('frames_encoded', math.ceil((a.seconds + 2) * s['symbol_rate'] / 22194) + 20)
    report.update(decode.vcm_match(frames, report['frame_modcods'], pattern=a.pattern,
                                  hold_frames=a.hold, max_frames=upper, callsign=s['callsign']))
    payload_checks = decode.vcm_checks(report)
    checks = dict(payload_checks, **board_checks(s))
    report.update(checks=checks, failed_checks=[key for key, passed in checks.items() if not passed],
                  payload_passed=all(payload_checks.values()), passed=all(checks.values()),
                  pattern=list(a.pattern), hold_frames=a.hold, callsign=s['callsign'],
                  board=s.get('board_result'))
    path = a.out / 'report.json'
    before = a.out / 'report-before.json'
    if path.exists() and not before.exists():
        before.write_bytes(path.read_bytes())
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('passed', 'payload_passed', 'failed_checks', 'frames_per_modcod',
                     'first_frame', 'last_frame', 'host_model_exact', 'contiguous', 'pattern_complete', 'compared_bytes')}))
    return 0 if report['passed'] else 1


def recheck(a, s):
    raw_bb = (a.out / 'frames.bbf').read_bytes()
    frames = decode.parse_bbframes(raw_bb)
    report = json.loads((a.out / 'report.json').read_text()) if (a.out / 'report.json').exists() else {}
    report.update(decode.summarize((a.out / 'leandvb.log').read_text(), frames,
                                  b''.join(frame['payload'] for frame in frames)))
    return evaluate(a, s, report, frames)


def verify(a, s, cfg, raw):
    if not raw.stat().st_size or raw.stat().st_size % 4:
        raise ValueError('capture needs complete int16 I/Q samples')
    data = np.memmap(raw, dtype='<i2', mode='r').reshape(-1, 2)
    cf = a.out / 'capture.cf32'
    peak, square_sum = 0, 0.0
    with cf.open('wb') as sink:
        for start in range(0, len(data), 1 << 20):
            block = data[start:start + (1 << 20)].astype(np.float32)
            peak = max(peak, int(np.abs(block).max()))
            square_sum += float(np.sum(block.astype(np.float64) ** 2))
            z = (block[:, 0] + (-1j if a.conj else 1j) * block[:, 1]) / a.scale
            z.astype('<c8').tofile(sink)
        np.zeros(4096, '<c8').tofile(sink)
    del data
    bb = a.out / 'frames.bbf'
    try:
        report, _ = decode.decode(cf, sample_rate=s['sample_rate'], symbol_rate=s['symbol_rate'],
                                 alpha=.35, receiver=a.receiver, output=a.out / 'payload.bin',
                                 bb_output=bb, log=a.out / 'leandvb.log', timeout=600)
    finally:
        if not a.keep:
            cf.unlink(missing_ok=True)  # raw IQ and receiver evidence suffice to reproduce failures
    frames = decode.parse_bbframes(bb.read_bytes())
    report.update(adc_peak=peak, adc_rms=math.sqrt(square_sum / (raw.stat().st_size // 2)),
                  e200=cfg, pattern=list(a.pattern), hold_frames=a.hold,
                  callsign=s['callsign'], receiver_flush_samples=4096)
    result = evaluate(a, s, report, frames)
    if not result and not a.keep:
        raw.unlink()
    return result


def main(argv=None):
    p = parser()
    a = p.parse_args(argv)
    try:
        s = settings(a)
        if a.plan:
            print(json.dumps(s, indent=2))
            return 0
        if a.key == (a.decode_only or a.recheck) or (a.decode_only and a.recheck):
            p.error('choose --decode-only, --recheck or explicit --key (coordinator, closed box only)')
        a.out.mkdir(parents=True, exist_ok=True)
        raw = a.out / 'capture.i16'
        saved = a.out / 'settings.json'
        cleanup_ok = True
        if a.decode_only or a.recheck:
            if saved.exists():
                stored = json.loads(saved.read_text())
                for key in ('pattern', 'hold_frames', 'symbol_rate', 'sample_rate', 'axes'):
                    if stored.get(key, False if key == 'axes' else None) != s[key]:
                        raise ValueError(f'{key} differs from saved settings')
                s.update(stored)
            else:
                s['callsign'] = morse.callsign(a.callsign or '')
            cfg = {'decode_only': True}
        else:
            if raw.exists() or saved.exists():
                raise ValueError('choose a fresh output directory for a new capture')
            try:
                cfg = prepare(a, s)
                saved.write_text(json.dumps(s, indent=2) + '\n')
                capture(a, s, raw)
            finally:
                cleanup_ok = restore(a, s)
                saved.write_text(json.dumps(s, indent=2) + '\n')
        result = recheck(a, s) if a.recheck else verify(a, s, cfg, raw)
        return result if cleanup_ok else (result or 1)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(json.dumps({'error': str(exc)}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
