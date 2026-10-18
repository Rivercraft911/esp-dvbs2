"""Axis OTA coordinator guards, exact verification and unconditional cleanup. No devices."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('e200_vcm', ROOT/'examples/over-the-air/e200_vcm.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


def args(out, *extra):
    return [str(out), '--axes', '--pattern', '4,11,13,17', '--hold', '1',
            '--rate', '5m', '--fs', '10000000', '--seconds', '30', *extra]


def good_board():
    return dict(cmd='dvb', status='ESP_OK', vcm=True, vcm_axes=True, fill_kernel='pie-axes5',
                dac_rate=40000000, symbol_rate=5000000, modcod=4, payload_source='xorshift32',
                callsign='N0CALL', encoder_failed=False, underruns=0, fatal_late=0,
                late_fills=0, late_switches=0, frames_encoded=64)


def test_plan_and_guards():
    with patch.object(ota.subprocess, 'run', side_effect=AssertionError('unexpected device call')), \
            patch.object(ota.subprocess, 'Popen', side_effect=AssertionError('unexpected device process')):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            assert ota.main(args('unused', '--plan')) == 0
        plan = json.loads(output.getvalue())
        assert plan['axes'] and plan['sample_rate'] == 10e6 and plan['burst_samples'] == 0
        assert plan['board_setup'] == ['info', 'spiinput off', 'vcm axes on', 'vcm 1 4 11 13 17']
        assert plan['board_command'] == 'dvb 2420 4 5m 30 4 128'
        assert plan['board_cleanup'] == ['vcm off', 'vcm axes off']
        for extra in ([], ['--key', '--decode-only'], ['--key', '--recheck']):
            with contextlib.redirect_stderr(io.StringIO()):
                try:
                    ota.main(args('unused', *extra))
                except SystemExit as exc:
                    assert exc.code == 2
                else:
                    raise AssertionError('authorization combination accepted')
        for bad in (['--rate', '10m'], ['--rate', '1m'], ['--fs', '9000000']):
            with contextlib.redirect_stderr(io.StringIO()):
                assert ota.main(args('unused', '--plan', *bad)) == 2
        no_axes = ota.parser().parse_args(['unused', '--pattern', '4,11,13,17'])
        try:
            ota.settings(no_axes)
        except ValueError:
            pass
        else:
            raise AssertionError('mixed 5m accepted without axes')
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stderr(io.StringIO()):
            for mode in ('--decode-only', '--recheck'):
                # Missing saved files fail locally; no cleanup command may touch a device.
                assert ota.main(args(tmp, mode, '--callsign', 'N0CALL')) == 2


def test_rx_inhibition():
    calls = []
    def attr(*parts):
        calls.append(parts)
        return {'powerdown':'1', 'frequency':'2420000000', 'sampling_frequency':'10000000',
                'rf_bandwidth':'7762500', 'hardwaregain':'30'}.get(parts[-1], '')
    with patch.object(ota, 'attr', side_effect=attr):
        cfg = ota.configure(2420e6, 10e6, 7762500, 30)
    assert calls[0] == ('-o','-c','ad9361-phy','altvoltage1','powerdown','1')
    assert ('-o','-c','ad9361-phy','altvoltage1','powerdown','0') not in calls
    assert sum(parts[-2:] == ('raw','0') for parts in calls) == 4
    assert cfg['tx_lo_powerdown'] == '1' and cfg['fs'] == '10000000'


def reply(a, commands, fail_cleanup=False, wrong_axes=False):
    def call(argv, **kwargs):
        assert argv[1] == str(ROOT/'host/board.py')
        command = argv[2]
        commands.append(command)
        if command == 'info':
            row = dict(callsign='N0CALL')
        elif command == 'spiinput off':
            row = dict(input='off')
        elif command == 'vcm axes on':
            row = dict(axes=not wrong_axes)
        elif command == 'vcm off':
            if fail_cleanup:
                return subprocess.CompletedProcess(argv, 1, '', 'reset failed')
            row = dict(enabled=False)
        elif command == 'vcm axes off':
            row = dict(axes=False)
        else:
            assert command.startswith('vcm ')
            row = dict(enabled=True, axes=a.axes, pattern=list(a.pattern), hold_frames=a.hold)
        return subprocess.CompletedProcess(argv, 0, json.dumps(row)+'\n', '')
    return call


def test_prepare_and_restore():
    with tempfile.TemporaryDirectory() as tmp:
        a = ota.parser().parse_args(args(tmp, '--key'))
        s, commands = ota.settings(a), []
        with patch.object(ota.subprocess, 'run', side_effect=reply(a, commands)), \
                patch.object(ota, 'configure', return_value={'mock': True}) as config:
            ota.prepare(a, s)
        assert commands == s['board_setup'] and config.call_count == 1
        commands.clear()
        with patch.object(ota.subprocess, 'run', side_effect=reply(a, commands, wrong_axes=True)), \
                patch.object(ota, 'configure', side_effect=AssertionError('configured after bad axis ACK')):
            try:
                ota.prepare(a, ota.settings(a))
            except RuntimeError:
                pass
            else:
                raise AssertionError('bad axis ACK accepted')
        commands.clear()
        with patch.object(ota.subprocess, 'run', side_effect=reply(a, commands, fail_cleanup=True)):
            assert not ota.restore(a, s)
        assert commands == ['vcm off', 'vcm axes off']
        assert not s['cleanup'][0]['passed'] and s['cleanup'][1]['passed']


def test_finally():
    for failure in ('none', 'prepare', 'capture', 'cleanup'):
        with tempfile.TemporaryDirectory() as tmp:
            a = ota.parser().parse_args(args(tmp, '--key'))
            commands = []
            def prepare(_a, s):
                if failure == 'prepare':
                    raise RuntimeError('mock prepare failure')
                s['callsign'] = 'N0CALL'
                return {'mock': True}
            def capture(_a, s, _raw):
                if failure == 'capture':
                    raise RuntimeError('mock capture failure')
                s['board_result'] = good_board()
            with patch.object(ota, 'prepare', side_effect=prepare), \
                    patch.object(ota, 'capture', side_effect=capture), \
                    patch.object(ota, 'verify', return_value=0) as verify, \
                    patch.object(ota.subprocess, 'run', side_effect=reply(a, commands, failure == 'cleanup')), \
                    contextlib.redirect_stderr(io.StringIO()):
                result = ota.main(args(tmp, '--key'))
            assert result == {'none': 0, 'prepare': 2, 'capture': 2, 'cleanup': 1}[failure]
            assert commands == ['vcm off', 'vcm axes off']
            assert verify.call_count == (failure not in ('prepare', 'capture'))
            assert Path(tmp, 'cleanup.json').exists()


def test_streaming_capture():
    with tempfile.TemporaryDirectory() as tmp:
        a = ota.parser().parse_args(args(tmp, '--key'))
        s = dict(ota.settings(a), callsign='N0CALL')
        events = []

        class Receiver:
            returncode = None

            def __init__(self, command, stdout, stderr):
                assert command[command.index('-s')+1] == '340000000'
                assert command[command.index('-b')+1] == '1048576'
                # Sparse file: exercise the full 30-second size check without IQ data.
                stdout.truncate(340000000 * 4)
                events.append('rx_start')

            def poll(self):
                return self.returncode

            def wait(self, timeout):
                events.append('rx_complete')
                self.returncode = 0
                return 0

        def key(command, **kwargs):
            assert command[2] == 'dvb 2420 4 5m 30 4 128'
            events.append('key')
            return subprocess.CompletedProcess(command, 0, json.dumps(good_board())+'\n', '')

        with patch.object(ota.subprocess, 'Popen', side_effect=Receiver), \
                patch.object(ota.subprocess, 'run', side_effect=key), \
                patch.object(ota.time, 'sleep'):
            ota.capture(a, s, Path(tmp, 'capture.i16'))
        assert events == ['rx_start', 'key', 'rx_complete']
        assert s['frames_encoded'] == 64


def test_payload_and_board_checks():
    # Reuse the saved-dump model generator and exercise the coordinator's real recheck.
    import check_vcm_dump as model
    expected = model.expected_frames([4,11,13,17], 16, 1, 'N0CALL')
    frames = expected[2:14]
    status = 'FRAMELOCK 1\n' + ''.join(
        f'S2FEC modcod={f["modcod"]} short=0 pilots=1 ldpc_flips=0 '
        'ldpc_checks_before=0 ldpc_checks_after=0 bch_errors=0\n' for f in frames)
    with tempfile.TemporaryDirectory() as tmp:
        a = ota.parser().parse_args(args(tmp, '--recheck'))
        s = dict(ota.settings(a), callsign='N0CALL', frames_encoded=16, board_result=good_board(),
                 cleanup=[dict(command=cmd, passed=True) for cmd in ('vcm off','vcm axes off')])
        Path(tmp, 'frames.bbf').write_bytes(b''.join(f['bbf'] for f in frames))
        Path(tmp, 'leandvb.log').write_text(status)
        with contextlib.redirect_stdout(io.StringIO()):
            assert ota.recheck(a, s) == 0
        report = json.loads(Path(tmp, 'report.json').read_text())
        assert report['host_model_exact'] and report['contiguous'] and report['pattern_complete']
        assert report['frames_per_modcod'] == {'4': 3, '11': 3, '13': 3, '17': 3}
        for key, value in [('underruns',1),('late_fills',1),('late_switches',1),('fatal_late',1),
                           ('vcm_axes',False),('fill_kernel','pie'),('encoder_failed',True)]:
            bad = dict(s, board_result=dict(good_board(), **{key:value}))
            assert not all(ota.board_checks(bad).values())
        assert not all(ota.board_checks(dict(s, cleanup=s['cleanup'][:1])).values())
        bad = dict(s, board_result=dict(good_board(), underruns=1))
        with contextlib.redirect_stdout(io.StringIO()):
            assert ota.recheck(a, bad) == 1
        report = json.loads(Path(tmp, 'report.json').read_text())
        assert report['payload_passed'] and not report['passed']
        assert 'board_no_dummies' in report['failed_checks']


if __name__ == '__main__':
    test_plan_and_guards()
    test_rx_inhibition()
    test_prepare_and_restore()
    test_finally()
    test_streaming_capture()
    test_payload_and_board_checks()
    print('E200 VCM axes: plan/key guards, pre-key ACK, exact per-mode payload, counters and both cleanup commands: ok; no devices')
