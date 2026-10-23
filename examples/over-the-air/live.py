#!/usr/bin/env python3
"""Coordinator-only USB live VCM demo. --plan/--recheck never open devices."""
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ota = load('live_ota', Path(__file__).with_name('check.py'))
send = load('live_usb_send', ota.ROOT/'examples/usb-send/send.py')
DEFAULT_SCHEDULE = send.control_schedule('0:4,3:7,6:11,9:4')


def parser():
    p = ota.parser()
    p.description = __doc__
    p.set_defaults(backend='libiio', rate='5m', modcod=4, gain=4, rx_gain=30)
    p.add_argument('--schedule', type=send.control_schedule, default=DEFAULT_SCHEDULE,
                   help='seconds after opening ID:MODCOD (default 0:4,3:7,6:11,9:4)')
    return p


def settings(a):
    if (a.rate != '5m' or a.backend != 'libiio' or a.burst or a.upsample != 1 or
            a.pattern or a.dry_run or a.modcod != a.schedule[0][1] or
            any(mc not in (4, 7, 11) for _, mc in a.schedule)):
        raise ValueError('live demo needs libiio streaming at 5m, QPSK MODCODs 4/7/11, and --modcod matching schedule start')
    if a.schedule[-1][0]+3 >= a.seconds:
        raise ValueError('schedule needs at least 3 seconds after the last switch')
    s = ota.settings(a)
    s.update(schedule=a.schedule, offered_payload_mbps=2.0)
    return s


def plan(a, s):
    result = ota.plan(a, s)
    result.update(board_setup=['vcm on', 'spiinput usb gcs'],
                  board_cleanup=['spiinput off', 'vcm off'],
                  usb_sender='examples/usb-send/send.py:stream', schedule=a.schedule,
                  steps=['Validate stored callsign and schedule fits between both IDs.',
                         'Start the existing receive-only libiio worker; require TX inhibit readbacks and pre-roll.',
                         'Stream unique GCS bytes at 2 Mb/s; insert timed PV type-3 controls on the same USB connection.',
                         'Decode with LeanDVB; require every ordered MODCOD run and exact contiguous sent payload.',
                         'Save sent.bin, sender.json, frames.bbf, control times/offsets, capture evidence and report.json.'])
    return result


def sender_checks(sender, a, call):
    send.check_run(sender['sender'], sender['records'])
    row, = [r for r in sender['records'] if 'usb' in r]
    expected = dict(cmd='dvb', status='ESP_OK', modcod=a.modcod, callsign=call,
                    symbol_rate=5000000, payload_source='usb', vcm=True, vcm_axes=False,
                    freq_mhz=a.mhz, seconds=a.seconds, gain=a.gain, peak_level=a.peak,
                    control_rejected=0, controls=len(a.schedule))
    controls = sender['sender']['control_log']
    timing_valid = len(controls) == len(a.schedule) and all(
        0 <= c['payload_bytes'] <= sender['sender']['payload_bytes'] and
        c['completed_seconds'] >= c['seconds'] >= (send.id_seconds(call)+t if i else 0)-1e-6 and
        (not i or (c['seconds'] >= controls[i-1]['completed_seconds'] and
                   c['payload_bytes'] > controls[i-1]['payload_bytes']))
        for i, ((t, _), c) in enumerate(zip(a.schedule, controls)))
    if (any(row.get(k) != v for k, v in expected.items()) or not timing_valid or
            not sender['sender'].get('complete') or sender['sender'].get('recovery') or
            row.get('vcm_changes') != len(a.schedule)-1 or
            [c['modcod'] for c in controls] != [mc for _, mc in a.schedule]):
        raise ValueError('USB live mode, controls or completion mismatch; see sender.json')
    return row


def hardware(a, s, out):
    import board
    process = None
    sender = dict(command=ota.tx_command(a), setup=[], sender=send.sender_stats(), records=[])
    # Longer than the bounded 2 Mb/s send window, including producer read-ahead.
    source = out/'input.bin'
    source.write_bytes(hashlib.shake_256(b'esp-dvbs2 live VCM demo v1').digest(a.seconds*250000+65536))
    def save():
        (out/'sender.json').write_text(json.dumps(sender, indent=2)+'\n')
        (out/'board.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in sender['setup']+sender['records']))
        with source.open('rb') as data:
            (out/'sent.bin').write_bytes(data.read(sender['sender']['payload_bytes']))
    with board.reserve_port(a.port), send.open_port(a.port) as link, (out/'iio.log').open('w') as log:
        def command(text):
            rows = []
            try:
                send.collect(link, text, records=rows)
            finally:
                sender['setup'].extend(dict(command=text, **row) for row in rows)
            return [r for r in rows if not r.get('done')]
        started = False
        try:
            call = ota.stored_callsign(command('info'), a.callsign, a.seconds)
            opening = send.id_seconds(call)
            if 2*opening+a.schedule[-1][0]+3 >= a.seconds:
                raise ValueError('stored callsign IDs leave too little time for schedule; increase --seconds')
            for text, field, value in (('vcm on', 'enabled', True), ('spiinput usb gcs', 'wire', 'usb')):
                rows = command(text)
                if not rows or rows[-1].get(field) != value:
                    raise RuntimeError(f'missing setup acknowledgement: {text}')
                if text == 'vcm on' and (rows[-1].get('pattern') != [] or rows[-1].get('mixed') is not False):
                    raise RuntimeError('need live QPSK controls, without a firmware pattern')
                if text == 'spiinput usb gcs' and rows[-1].get('input') != 'gcs':
                    raise RuntimeError('need USB GCS input')
            (out/'settings.json').write_text(json.dumps(s, indent=2)+'\n')
            process = subprocess.Popen(ota.capture_command(a, out/'settings.json'), stdout=log, stderr=subprocess.STDOUT)
            ota.wait_ready(process, out, s)
            print(f'Capture ready; {call}; {sender["command"]}', flush=True)
            started = True
            with source.open('rb') as data:
                send.stream(link, data, cmd=sender['command'], kind=2, seconds=a.seconds,
                            mbps=2.0, opening_id=opening, schedule=a.schedule, rate=a.rate,
                            modcod=a.modcod, stats=sender['sender'], rows=sender['records'])
            save()
            row = sender_checks(sender, a, call)
            time.sleep(s['post_seconds'])
            (out/'stop').touch()
            code = process.wait(timeout=60)
            capture = json.loads((out/'capture.json').read_text())
            if code or capture.get('complete') is not True or capture.get('overflows'):
                raise RuntimeError('incomplete SDR capture; see capture.json and iio.log')
            return call, row, capture
        finally:
            (out/'stop').touch()
            ota.stop(process)
            cleanup = []
            for text, field, value in (('spiinput off', 'input', 'off'),
                    ('vcm off', 'enabled', False)):
                try:
                    if started and (not sender['sender']['complete'] or sender['sender']['recovery']):
                        raise RuntimeError('console handoff unconfirmed; no console write attempted')
                    rows = command(text)
                    if not rows or rows[-1].get(field) != value:
                        raise RuntimeError('missing cleanup acknowledgement')
                    cleanup.append(dict(command=text, passed=True))
                except Exception as exc:
                    cleanup.append(dict(command=text, passed=False, error=str(exc)))
            (out/'cleanup.json').write_text(json.dumps(cleanup, indent=2)+'\n')
            save()
            if not all(r['passed'] for r in cleanup):
                raise RuntimeError('live cleanup failed; see cleanup.json')


def analyze(out, a):
    s = json.loads((out/'settings.json').read_text())
    sender = json.loads((out/'sender.json').read_text())
    frames = ota.decode.parse_bbframes((out/'frames.bbf').read_bytes())
    r = ota.decode.summarize((out/'leandvb.log').read_text(), frames, b''.join(f['payload'] for f in frames))
    sent = (out/'sent.bin').read_bytes()
    r.update(ota.decode.live_vcm_match(frames, r['frame_modcods'], sent, sender['sender']['control_log']))
    checks = ota.decode.vcm_checks(r)
    checks.update(control_boundaries_valid=r['control_boundaries_valid'],
                  enough_frames=bool(r['segments']) and all(segment['frames'] >= a.min_frames for segment in r['segments']),
                  sent_byte_count=len(sent) == sender['sender']['payload_bytes'])
    try:
        call = ota.stored_callsign([r for r in sender['setup'] if r.get('cmd') == 'info' and not r.get('done')],
                                  a.callsign, a.seconds)
        row = sender_checks(sender, a, call)
        checks['sender_valid'] = True
    except (ValueError, KeyError) as exc:
        row = next((r for r in sender['records'] if 'usb' in r), {})
        checks['sender_valid'] = False
        r['sender_error'] = str(exc)
    capture = json.loads((out/'capture.json').read_text())
    cleanup = json.loads((out/'cleanup.json').read_text())
    checks.update(capture_complete=capture.get('complete') is True and not capture.get('overflows'),
                  cleanup_valid=([c.get('command') for c in cleanup] == ['spiinput off', 'vcm off'] and
                                 all(c.get('passed') is True for c in cleanup)))
    r.update(passed=all(checks.values()), checks=checks, failed_checks=[k for k,v in checks.items() if not v],
             sent_sha256=hashlib.sha256(sent).hexdigest(), sent_bytes=len(sent),
             control_log=sender['sender']['control_log'], schedule=s['schedule'],
             board=row, capture=capture, payload_bit_errors=0 if r['host_model_exact'] else None)
    return r


def main(argv=None):
    p = parser()
    a = p.parse_args(argv)
    try:
        if a.recheck:
            if a.key or a.plan:
                raise ValueError('--recheck cannot be combined with --key/--plan')
            source = a.recheck.resolve()
            saved = json.loads((source/'settings.json').read_text())
            a.schedule = tuple(map(tuple, saved['schedule']))
            sender = json.loads((source/'sender.json').read_text())
            row, = [r for r in sender['records'] if 'usb' in r]
            a.modcod, a.mhz, a.seconds, a.gain, a.peak = (row[k] for k in ('modcod', 'freq_mhz', 'seconds', 'gain', 'peak_level'))
            result = analyze(source, a)
        else:
            s = settings(a)
            if a.plan:
                print(json.dumps(plan(a, s), indent=2))
                return 0
            if not a.key or not a.uri:
                p.error('hardware requires explicit --key AND --uri; use --plan or --recheck')
            if not shutil.which(a.receiver) or not shutil.which(a.iio_python):
                raise ValueError('need LeanDVB and the libiio Python; use --receiver/--iio-python')
            import numpy, scipy, plotstyle  # noqa: F401; fail before devices
        out = (a.out or ota.ROOT/'notes'/dt.datetime.now().strftime('live-vcm-%Y%m%d-%H%M%S')).resolve()
        out.mkdir(parents=True, exist_ok=False)
        if a.recheck:
            names = ('settings.json', 'sender.json', 'sent.bin', 'frames.bbf', 'leandvb.log', 'capture.json', 'cleanup.json')
            result.update(source=str(source), source_sha256={name: hashlib.sha256((source/name).read_bytes()).hexdigest() for name in names})
        else:
            (out/'plan.json').write_text(json.dumps(plan(a, s), indent=2)+'\n')
            (out/'revision.json').write_text(json.dumps(dict(coordinator=subprocess.check_output(
                ['git', 'rev-parse', 'HEAD'], cwd=ota.ROOT, text=True).strip(), dirty=bool(subprocess.check_output(
                ['git', 'status', '--porcelain'], cwd=ota.ROOT, text=True))), indent=2)+'\n')
            result = dict(passed=False)
            try:
                if shutil.disk_usage(out).free < ota.capture_bytes(s)*1.5+100e6:
                    raise ValueError('insufficient disk for bounded capture')
                hardware(a, s, out)
                plotted = ota.spectrum(out/'capture.cfile', s['sample_rate'], s, out)
                ota.decode.decode(out/'capture.cfile', sample_rate=s['sample_rate'], symbol_rate=s['symbol_rate'],
                    receiver=a.receiver, output=out/'payload.bin', bb_output=out/'frames.bbf',
                    log=out/'leandvb.log', timeout=a.timeout)
                result = analyze(out, a)
                result['spectrum'] = plotted
            except (Exception, KeyboardInterrupt) as exc:
                result.update(passed=False, error=str(exc) or 'interrupted; firmware retains its timed key window')
        (out/'report.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
        print(json.dumps({k: result.get(k) for k in ('passed', 'failed_checks', 'error', 'segments')}))
        print(f'Evidence: {out}')
        return 0 if result['passed'] else 1
    except (OSError, ValueError) as exc:
        p.error(str(exc))


if __name__ == '__main__':
    raise SystemExit(main())
