"""Live USB controls, sent-byte verification and coordinator lifecycle. No devices."""
import argparse
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

import numpy as np
from test_ota import load, ota, rejects
from test_usb_send import Peer
import dvbs2

live = load('live_demo', ota.ROOT/'examples/over-the-air/live.py')
send, decode = live.send, ota.decode


def test_schedule():
    schedule = send.control_schedule('0:4,.06:7,.12:11,.18:4')
    peer = Peer()
    stats, rows = send.stream(peer, io.BytesIO(bytes(30000)), cmd='dvbpace 4 5m 4',
                             kind=2, seconds=4, schedule=schedule, rate='5m', mbps=.25,
                             opening_id=.04)
    send.check_run(stats, rows)
    assert peer.controls == [4, 7, 11, 4] and stats['payload_bytes'] == 30000
    assert [c['modcod'] for c in stats['control_log']] == peer.controls
    for (t, mc), c in zip(schedule[1:], stats['control_log'][1:]):
        assert c['seconds'] >= t+.04 and c['completed_seconds'] >= c['seconds']
        assert c['payload_bytes'] % send.PAYLOAD == 0
    # A short source must fail, but still finish the PV/console handoff.
    stats, rows = send.stream(Peer(), io.BytesIO(b'short'), cmd='dvbpace 4 5m 4',
                             kind=2, seconds=4, schedule=schedule, rate='5m')
    assert stats['terminal_nop'] and stats['errors'] == ['source/run ended before complete MODCOD schedule']
    rejects(send.check_run, stats, rows)
    for value in ('', '0:4', '1:4,2:7', '0:4,0:7', '0:4,1:4', '0:4,nan:7',
                  '0:4,inf:7', '0:4,-1:7', '0:4,1:18', '0:4,1:7:11'):
        try:
            send.control_schedule(value)
        except argparse.ArgumentTypeError:
            pass
        else:
            raise AssertionError(value)
    # Invalid future entries must be caught before the USB port opens.
    with tempfile.TemporaryDirectory() as tmp, patch.object(send, 'open_port', side_effect=AssertionError('opened USB')):
        source = Path(tmp)/'input'; source.write_bytes(b'data')
        for schedule in ('0:4,1:13', '0:7,1:11', '0:4,10:7'):
            with contextlib.redirect_stderr(io.StringIO()):
                try:
                    send.main(['send', str(source), '--kind', 'gcs', '--rate', '5m',
                               '--schedule', schedule, '--report', str(Path(tmp)/'report.json')])
                except SystemExit as exc:
                    assert exc.code == 1
                else:
                    raise AssertionError('bad schedule accepted')


def fixture(out):
    a = live.parser().parse_args(['--uri', 'ip:fake'])
    modes = [mc for mc in (4, 7, 11, 4) for _ in range(12)]
    sizes = [send.mode(mc, '5m')[0] for mc in modes]
    data = hashlib.shake_256(b'live test bytes').digest(sum(sizes))
    frames, offset, controls = [], 0, []
    for i, (mc, size) in enumerate(zip(modes, sizes)):
        if i % 12 == 0:
            controls.append(dict(modcod=mc, payload_bytes=0 if i == 0 else offset//1316*1316,
                                 seq=i, seconds=0 if i == 0 else 5.22+i/4,
                                 completed_seconds=.01 if i == 0 else 5.23+i/4))
        bbf = dvbs2.bbframe(np.unpackbits(np.frombuffer(data[offset:offset+size], np.uint8)),
                           dvbs2.RATES[mc-1], ccm=0)
        frames.extend(decode.parse_bbframes(np.packbits(bbf).tobytes()[:10+size]))
        offset += size
    messages = (len(data)+1315)//1316
    stats = dict(send.sender_stats(), messages=messages, controls=4, control_log=controls,
                 payload_bytes=len(data), next_seq=messages+4, source_eof=True, terminal_nop=True, complete=True)
    usb = dict(messages=messages, payload_bytes=len(data), ended=True, nops=1, next_seq=messages+5,
               ts_packets=0, crc_chain=0, short=0, header=0, crc=0, sync=0, lost=0, back=0,
               wrong_type=0, partial_bytes=0, driver_pending=0, us=18000000, mbps=2)
    row = dict(cmd='dvb', usb=usb, controls=4, control_rejected=0, vcm=True, vcm_axes=False,
               vcm_changes=3, modcod=4, callsign='N0CALL', status='ESP_OK', encoder_failed=False,
               payload_source='usb', symbol_rate=5000000, freq_mhz=2420, seconds=30,
               gain=4, peak_level=128, frames_encoded=len(frames), underruns=100,
               late_fills=0, late_switches=0, fatal_late=0, min_fill_slack_cycles=1,
               payload_bytes_per_frame=4016, frame_symbols=33282)
    sender = dict(sender=stats, records=[dict(cmd='usbinput', ready=True), row],
                  setup=[dict(cmd='info', callsign='N0CALL')])
    for name, value in (('settings.json', live.settings(a)), ('sender.json', sender),
                        ('capture.json', dict(complete=True, overflows=None)),
                        ('cleanup.json', [dict(command=c, passed=True) for c in
                         ('spiinput off', 'vcm off', 'vcm axes off')])):
        (out/name).write_text(json.dumps(value))
    (out/'sent.bin').write_bytes(data)
    (out/'frames.bbf').write_bytes(b''.join(f['header']+f['payload'] for f in frames))
    status = 'FRAMELOCK 1\n'+'\n'.join(f'S2FEC modcod={mc} short=0 pilots=1 ldpc_flips=0 '
        'ldpc_checks_before=0 ldpc_checks_after=0 bch_errors=0' for mc in modes)
    (out/'leandvb.log').write_text(status)
    return a, frames, modes, data, sender


def test_verifier():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        a, frames, modes, data, sender = fixture(out)
        result = live.analyze(out, a)
        assert result['passed'], result
        assert [s['frames'] for s in result['segments']] == [12]*4
        assert [s['modcod'] for s in result['segments']] == [4, 7, 11, 4]
        assert result['compared_bytes'] == len(data)
        controls = sender['sender']['control_log']
        # Edge loss is fine when all ordered runs remain. Interior losses fail.
        assert decode.live_vcm_match(frames[2:-2], modes[2:-2], data, controls)['host_model_exact']
        faults = [(frames[:20]+frames[21:], modes[:20]+modes[21:]),
                  (frames[:20]+frames[19:], modes[:20]+modes[19:]),
                  (frames[:20]+frames[21:22]+frames[20:21]+frames[22:], modes),
                  (frames, modes[:-1]), ([], [])]
        corrupt = dict(frames[20], payload=b'X'+frames[20]['payload'][1:])
        faults.append((frames[:20]+[corrupt]+frames[21:], modes))
        corrupt = dict(frames[20], header=b'X'+frames[20]['header'][1:])
        faults.append((frames[:20]+[corrupt]+frames[21:], modes))
        for fs, ms in faults:
            assert not decode.live_vcm_match(fs, ms, data, controls)['host_model_exact']
        assert not decode.live_vcm_match(frames[:-12], modes[:-12], data, controls)['pattern_complete']
        wrong = copy.deepcopy(controls); wrong[1]['payload_bytes'] -= 20000
        assert not decode.live_vcm_match(frames, modes, data, wrong)['control_boundaries_valid']
        for key, value in (('control_rejected', 1), ('vcm_changes', 2), ('payload_source', 'xorshift32'), ('late_fills', 1)):
            bad = copy.deepcopy(sender); bad['records'][1][key] = value
            rejects(live.sender_checks, bad, a, 'N0CALL')
        bad = copy.deepcopy(sender); bad['sender']['control_log'][1]['seconds'] = 0
        rejects(live.sender_checks, bad, a, 'N0CALL')
        # Saved evidence replay is byte-for-byte read-only and needs no drivers.
        before = {p.name:p.read_bytes() for p in out.iterdir()}
        with contextlib.redirect_stdout(io.StringIO()):
            assert live.main([str(out/'audit'), '--recheck', str(out)]) == 0
        assert before == {name:(out/name).read_bytes() for name in before}
        assert set(json.loads((out/'audit/report.json').read_text())['source_sha256']) == set(before)
        (out/'frames.bbf').write_bytes(b''.join(f['header']+f['payload'] for f in frames[:20]+frames[21:]))
        (out/'leandvb.log').write_text('\n'.join((out/'leandvb.log').read_text().splitlines()[:21]+
                                              (out/'leandvb.log').read_text().splitlines()[22:]))
        with contextlib.redirect_stdout(io.StringIO()):
            assert live.main([str(out/'gap-audit'), '--recheck', str(out)]) == 1


def test_guards_and_hardware():
    script = str(ota.ROOT/'examples/over-the-air/live.py')
    planned = subprocess.check_output([sys.executable, '-S', script, '--plan', '--uri', 'ip:fake'])
    assert json.loads(planned)['board_setup'] == ['vcm axes off', 'vcm on', 'spiinput usb gcs']
    for extra in ([], ['--key'], ['--uri', 'ip:fake'], ['--plan', '--rate', '10m'],
                  ['--plan', '--burst', '16000000'], ['--plan', '--schedule', '0:4,3:13']):
        proc = subprocess.run([sys.executable, '-S', script, *extra], capture_output=True)
        assert proc.returncode == 2 and b'ModuleNotFoundError' not in proc.stderr
    for fault in (None, 'capture', 'setup', 'stream'):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            a, _, _, _, sender = fixture(out)
            sent = []
            def collect(link, text, records=None):
                sent.append(text)
                row = dict(cmd=text.split()[0])
                if text == 'info': row.update(callsign='N0CALL')
                elif text == 'vcm axes off': row.update(axes=False)
                elif text == 'vcm on': row.update(enabled=fault != 'setup', pattern=[], mixed=False)
                elif text == 'vcm off': row.update(enabled=False)
                elif text == 'spiinput usb gcs': row.update(input='gcs', wire='usb')
                elif text == 'spiinput off': row.update(input='off')
                else: raise AssertionError(text)
                records.extend([row, dict(cmd=row['cmd'], done=True)])
                return records
            def stream(link, source, **kw):
                sent.append(kw['cmd'])
                assert kw['schedule'] == live.DEFAULT_SCHEDULE and kw['mbps'] == 2
                kw['stats'].update(sender['sender']); kw['rows'].extend(sender['records'])
                if fault == 'stream': kw['stats']['complete'] = False
            class Process:
                def wait(self, **kw): return 0
                def poll(self): return 0
            with patch.dict(sys.modules, board=types.SimpleNamespace(reserve_port=lambda _: contextlib.nullcontext())), \
                    patch.object(send, 'open_port', return_value=contextlib.nullcontext()), \
                    patch.object(send, 'collect', side_effect=collect), patch.object(send, 'stream', side_effect=stream), \
                    patch.object(ota.subprocess, 'Popen', return_value=Process()), patch.object(live.ota, 'stop'), \
                    patch.object(live.ota, 'wait_ready', side_effect=RuntimeError('no IQ') if fault == 'capture' else None), \
                    patch.object(live.time, 'sleep'):
                if fault: rejects(live.hardware, a, live.settings(a), out)
                else: live.hardware(a, live.settings(a), out)
            if fault in ('capture', 'setup'):
                assert not any(c.startswith('dvb ') for c in sent)
            if fault == 'stream':
                assert sent[-1].startswith('dvb ')
                assert not any(c['passed'] for c in json.loads((out/'cleanup.json').read_text()))
            else:
                assert sent[-3:] == ['spiinput off', 'vcm off', 'vcm axes off']
            assert (out/'sender.json').exists() and (out/'sent.bin').exists()


def test_receiver(receiver):
    """Portable C encoder -> independent pulse -> LeanDVB -> live verifier."""
    from scipy.signal import upfirdn
    from test_dvbs2_c import NativeEncoder, U8P
    with tempfile.TemporaryDirectory() as tmp, NativeEncoder() as enc:
        out = Path(tmp)
        a, frames, modes, _, _ = fixture(out)
        symbols = []
        for frame, mc in zip(frames, modes):
            mode = enc.lib.dvbs2_find_mode(mc)
            assert enc.lib.dvbs2_fast_prepare(enc.fast, mode, 1, 2048|16) == 0
            payload = np.frombuffer(frame['payload'], np.uint8)
            phases = np.empty(33282, np.uint8)
            assert enc.lib.dvbs2_encode_frame(enc.fast, enc.ws, None, payload.ctypes.data_as(U8P),
                len(payload)*8, phases.ctypes.data_as(U8P)) == 0
            symbols.append(np.exp(1j*np.pi/4*phases))
        t = np.arange(-24, 25, dtype=float)/4
        alpha = .35
        taps = np.array([1+alpha*(4/np.pi-1) if x == 0 else
            (np.sin(np.pi*x*(1-alpha))+4*alpha*x*np.cos(np.pi*x*(1+alpha)))/
            (np.pi*x*(1-(4*alpha*x)**2)) for x in t])
        iq = out/'test.cfile'
        wave = upfirdn(taps, np.concatenate(symbols), up=4).astype('<c8')
        np.concatenate((wave, np.zeros(4096, '<c8'))).tofile(iq)
        decode.decode(iq, sample_rate=20e6, symbol_rate=5e6, receiver=receiver,
                      bb_output=out/'frames.bbf', log=out/'leandvb.log', timeout=90)
        result = live.analyze(out, a)
        assert result['passed'], result
        print('C encoder -> LeanDVB: '+json.dumps(dict(bytes=result['compared_bytes'], segments=result['segments'])))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receiver', type=Path)
    a = p.parse_args()
    test_schedule()
    test_verifier()
    test_guards_and_hardware()
    if a.receiver:
        test_receiver(a.receiver)
    print('Live VCM: timed USB controls, ordered runs, exact sent bytes, gap rejection, offline replay and cleanup pass')
