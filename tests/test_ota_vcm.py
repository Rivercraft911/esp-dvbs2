"""VCM uses the shared OTA capture, with strict patterns and cleanup. No devices."""
import contextlib
import json
from pathlib import Path
import sys
import tempfile
import types
from unittest.mock import patch

from test_ota import ota, rejects
from test_vcm_decode import frames


def test_plan_and_analysis():
    for pattern, hold in (((4, 7, 11), 4), ((4, 11, 13, 17), 1)):
        args = ota.parser().parse_args(['--backend', 'libiio', '--uri', 'ip:fake',
            '--pattern', ','.join(map(str, pattern)), '--hold', str(hold), '--plan'])
        settings = ota.settings(args)
        plan = ota.plan(args, settings)
        assert plan['board_setup'] == ['vcm axes off', f'vcm {hold} ' + ' '.join(map(str, pattern))]
        assert plan['board_command'] == 'dvb 2420 4 5m 30 0 128'
        rows, modes = frames(pattern, 36, hold)
        status = 'FRAMELOCK 1\n' + '\n'.join(
            f'S2FEC modcod={mc} short=0 pilots=1 ldpc_flips=0 ldpc_checks_before=0 '
            'ldpc_checks_after=0 bch_errors=0' for mc in modes[2:34])
        for bad in (False, True):
            actual = rows[2:34] if not bad else rows[2:16] + rows[17:35]
            report = ota.decode.summarize(status, actual, b''.join(r['payload'] for r in actual))
            raw = b''.join(r['header'] + r['payload'] for r in actual)
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                def receiver(*_args, **kw):
                    kw['bb_output'].write_bytes(raw)
                    return dict(report), b''.join(r['payload'] for r in actual)
                with patch.object(ota.decode, 'decode', side_effect=receiver):
                    result = ota.analyze(out/'unused', 10e6, 36, 'N0CALL', args, settings, out)
                assert result['passed'] is not bad, result
                assert result['pattern_complete']
    for extra in (['--rate', '10m'], ['--rate', '1m']):
        args = ota.parser().parse_args(['--pattern', '4,13', '--backend', 'libiio', *extra])
        rejects(ota.settings, args)


def test_cleanup():
    args = ota.parser().parse_args(['--backend', 'libiio', '--uri', 'ip:fake', '--pattern', '4,11,13,17', '--hold', '1'])
    settings = ota.settings(args)
    row = dict(cmd='dvb', status='ESP_OK', modcod=4, callsign='N0CALL', symbol_rate=5000000,
        payload_source='xorshift32', encoder_failed=False, underruns=0, frames_played=30, frames_encoded=32,
        freq_mhz=2420, seconds=30, gain=0, peak_level=128, vcm=True, vcm_axes=True,
        fill_kernel='pie-axes5', fatal_late=0, late_fills=0, late_switches=0)
    assert ota.board_result([row], args, 'N0CALL') == 32
    for key, value in (('underruns', 1), ('late_fills', 1), ('late_switches', 1), ('vcm_axes', False)):
        rejects(ota.board_result, [dict(row, **{key: value})], args, 'N0CALL')
    for fault in (None, 'capture', 'setup'):
        sent = []
        def command(port, text, **kw):
            sent.append(text)
            if text == 'info': return [dict(callsign='N0CALL')]
            if text.startswith('dvb '): return [row]
            if text == 'vcm off': return [dict(enabled=False)]
            if text.startswith('vcm axes'): return [dict(axes=False)]
            if text.startswith('vcm '):
                return [dict(pattern=[4, 11, 13, 17], hold_frames=1, enabled=fault != 'setup')]
            return [dict(cmd='spiinput')]
        board = types.SimpleNamespace(run=command, reserve_port=lambda _: contextlib.nullcontext())
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            class Process:
                def wait(self, **kw):
                    (out/'capture.json').write_text(json.dumps(dict(complete=True, overflows=None)))
                    return 0
                def poll(self): return 0
            with patch.dict(sys.modules, board=board), patch.object(ota.subprocess, 'Popen', return_value=Process()), \
                    patch.object(ota, 'wait_ready', side_effect=RuntimeError('no IQ') if fault == 'capture' else None), \
                    patch.object(ota.time, 'sleep'):
                if fault: rejects(ota.hardware, args, settings, out)
                else: assert ota.hardware(args, settings, out)[:2] == ('N0CALL', 32)
            assert sent[-2:] == ['vcm off', 'vcm axes off']
            assert all(r['passed'] for r in json.loads((out/'cleanup.json').read_text()))
            if fault: assert not any(s.startswith('dvb ') for s in sent)


if __name__ == '__main__':
    test_plan_and_analysis()
    test_cleanup()
    print('OTA VCM: libiio plan, exact pattern/gap checks, board deadlines and cleanup pass')
