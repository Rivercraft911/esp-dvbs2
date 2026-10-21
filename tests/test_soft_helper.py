"""External-process boundary tests; fake executables only, no radio or DSP."""
import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


decode = module('decode', ROOT / 'host/decode.py')
sys.modules['decode'] = decode
live = module('soft_live', ROOT / 'examples/live-video/receive.py')
SOFT = 'S2SOFT modcod=4 short=0 pilots=1 bch_errors=0 ldpc_parity=unavailable\n'
DRAIN = 'S2HELPER sent=1 received=1 pending=0 exit=0\n'


def executable(path, body):
    path.write_text('#!' + sys.executable + '\n' + body)
    path.chmod(0o755)
    return path


def running(pid):
    result = subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)],
                            capture_output=True, text=True, check=False)
    return bool(result.stdout.strip()) and not result.stdout.lstrip().startswith('Z')


class SoftHelperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.iq = self.folder / 'iq.cf32'
        self.iq.write_bytes(bytes(16))
        self.pidfile = self.folder / 'child.pid'
        self.helper = executable(self.folder / 'helper with spaces',
            'import os,signal,sys,time\n'
            'signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            'open(sys.argv[1],"w").write(str(os.getpid()))\n'
            'while True: time.sleep(.01)\n')

    def frontend(self, mode='ok', drain=True):
        header = b'\x60\0\0\0\0\x18\0\0\0'
        frame = header + bytes([decode.crc8(header)]) + b'abc'
        status = 'FRAMELOCK 1\n' + SOFT + (DRAIN if drain else '')
        return executable(self.folder / 'receiver',
            'import os,pathlib,subprocess,sys,time\n'
            f'helper={str(self.helper)!r}; pidfile={str(self.pidfile)!r}\n'
            'subprocess.Popen([helper,pidfile],stdout=subprocess.DEVNULL)\n'
            'while not pathlib.Path(pidfile).exists(): time.sleep(.001)\n'
            'sys.stdin.buffer.read()\n'
            f'sys.stderr.write({status!r}); sys.stderr.flush()\n'
            f'mode={mode!r}\n'
            'if mode=="timeout": time.sleep(20)\n'
            'if mode=="error": raise SystemExit(7)\n'
            f'if "--fd-bbf" in sys.argv: os.write(int(sys.argv[sys.argv.index("--fd-bbf")+1]),{frame!r})\n'
            'else: sys.stdout.buffer.write((b"G"+bytes(187))*2)\n')

    def assert_child_stopped(self):
        pid = int(self.pidfile.read_text())
        for _ in range(100):
            if not running(pid):
                return
            time.sleep(.01)
        self.fail(f'helper {pid} still running')

    def saved(self, receiver, **kwargs):
        return decode.decode(self.iq, sample_rate=4e6, symbol_rate=1e6,
                             receiver=receiver, ldpc_helper=self.helper, modcod=4, **kwargs)

    def test_default_argv_and_reports_unchanged(self):
        self.assertEqual(decode.receiver_command('rx',4e6,1e6,.35,9),
            ['rx','--standard','DVB-S2','--f32','-f','4000000.0','--sr','1000000.0',
             '--sampler','rrc','--rrc-rej','20','--fastdrift','--sr-tol','0.0005',
             '--roll-off','0.35','--ldpc-bf','100','--fd-bbf','9','--fd-info','2','-d','-d'])
        for status in ('', 'LDPCCORR = 0\nBCHCORR = 0\n',
                       'FRAMELOCK 1\nS2FEC modcod=4 short=0 pilots=1 ldpc_flips=2 '
                       'ldpc_checks_before=3 ldpc_checks_after=0 bch_errors=0\n'):
            report = decode.summarize(status, [], b'abc')
            self.assertNotIn('fec_backend',report)
            self.assertNotIn('helper',report)
        self.assertEqual(report['ldpc_bit_flips'],2)
        self.assertEqual(report['ldpc_checks_before'],3)
        self.assertEqual(report['ldpc_checks_after'],0)
        self.assertTrue(report['status_complete'])
        args = live.parser().parse_args(['--iq',str(self.iq),'--receiver','leandvb'])
        self.assertEqual(live.commands(args),(None,
            ['leandvb','--standard','DVB-S2','--f32','-f','20000000.0','--sr','10000000.0',
             '--roll-off','0.35','--sampler','rrc','--rrc-rej','20','--modcods','128',
             '--framesizes','1','--ldpc-bf','100','--fastdrift','--sr-tol','0.0005',
             '--fd-info','2','-v']))
        command = decode.receiver_command('rx',4e6,1e6,.35,9,ldpc_helper='x',modcod=4)
        self.assertNotIn('--ldpc-bf',command)
        self.assertEqual(command[command.index('--nhelpers')+1],'1')
        self.assertEqual(command[command.index('--modcods')+1],'16')
        self.assertEqual(command[command.index('--framesizes')+1],'1')

    def test_known_normal_mode_required_before_opening(self):
        receiver = self.frontend()
        for mc in (None,0,29,-1,True,13.0,(4,13)):
            with self.subTest(mc=mc), patch.object(Path,'open',side_effect=AssertionError('opened source')):
                with self.assertRaises(ValueError):
                    decode.decode(self.iq,sample_rate=4e6,symbol_rate=1e6,
                                  receiver=receiver,ldpc_helper=self.helper,modcod=mc)
        self.assertEqual(decode.receiver_command('rx',4e6,1e6,.35,9,modcod=13),
                         decode.receiver_command('rx',4e6,1e6,.35,9))
        for mc in (1,13,28):
            command=decode.receiver_command('rx',4e6,1e6,.35,9,ldpc_helper='x',modcod=mc)
            self.assertEqual(command[command.index('--modcods')+1],str(1<<mc))
        output=io.StringIO()
        args=['decode.py',str(self.iq),'--rate','4000000','--symbol-rate','1000000',
              '--stream','6','--ldpc-helper',str(self.helper),'--pattern','4,13']
        with patch.object(sys,'argv',args),contextlib.redirect_stdout(output):
            self.assertEqual(decode.main(),2)
        self.assertIn('without --pattern',output.getvalue())

    def test_nullable_metrics_and_strict_eof(self):
        report = decode.summarize(SOFT+DRAIN,[],b'',ldpc_helper='x')
        self.assertTrue(report['status_complete'])
        for key in ('ldpc_bit_flips','ldpc_checks_before','ldpc_checks_after','ldpc_residual_frames'):
            self.assertIsNone(report[key])
        self.assertIsNone(report['payload_fec']['ldpc_bit_flips'])
        self.assertEqual(report['bch_failed_frames'],0)
        for status in (SOFT, SOFT+DRAIN*2, SOFT+DRAIN.replace('pending=0','pending=1'),
                       SOFT+DRAIN.replace('exit=0','exit=1'), SOFT+DRAIN.replace('sent=1','sent=2'),
                       SOFT+DRAIN+'S2SOFT malformed\n', SOFT+DRAIN+'S2HELPER malformed\n',
                       SOFT+DRAIN+'S2FEC modcod=4 short=0 pilots=1 ldpc_flips=0 '
                       'ldpc_checks_before=0 ldpc_checks_after=0 bch_errors=0\n'):
            self.assertFalse(decode.summarize(status,[],b'',ldpc_helper='x')['status_complete'])
        status = SOFT+SOFT.replace('bch_errors=0','bch_errors=-1')+SOFT
        report = decode.summarize(status+DRAIN.replace('=1','=3'),[],b'',ldpc_helper='x')
        self.assertTrue(report['status_complete'])
        self.assertEqual(report['payload_fec']['bch_failed_frames'],1)

    def test_saved_eof_reaps_orphan_and_records_provenance(self):
        report, data = self.saved(self.frontend(),expected=b'abc')
        self.assertEqual(data,b'abc')
        self.assertTrue(report['status_complete'] and report['payload_match'])
        self.assertEqual(report['ldpc_helper'],str(self.helper.resolve()))
        self.assertEqual(len(report['receiver_sha256']),64)
        self.assert_child_stopped()

    def test_missing_eof_is_incomplete_and_reaps_orphan(self):
        report, _ = self.saved(self.frontend(drain=False))
        self.assertFalse(report['status_complete'])
        self.assert_child_stopped()

    def test_timeout_error_and_interrupt_cleanup(self):
        for mode, exception in (('timeout',subprocess.TimeoutExpired),('error',RuntimeError)):
            with self.subTest(mode=mode):
                self.pidfile.unlink(missing_ok=True)
                log = self.folder / 'diagnostics'
                with self.assertRaises(exception):
                    self.saved(self.frontend(mode),timeout=1,log=log)
                self.assertIn('S2SOFT',log.read_text())
                self.assert_child_stopped()
        self.pidfile.unlink()
        receiver = self.frontend('timeout')
        wait = subprocess.Popen.wait
        fired = False
        def interrupted(process,*args,**kwargs):
            nonlocal fired
            if not fired:
                fired = True
                while not self.pidfile.exists(): time.sleep(.001)
                raise KeyboardInterrupt()
            return wait(process,*args,**kwargs)
        with patch.object(subprocess.Popen,'wait',interrupted), self.assertRaises(KeyboardInterrupt):
            self.saved(receiver)
        self.assert_child_stopped()

    def test_stop_escalates_without_masking_timeout(self):
        child = subprocess.Popen([str(self.helper),str(self.pidfile)],start_new_session=True)
        while not self.pidfile.exists(): time.sleep(.001)
        decode.stop_decoder(child,timeout=.02)
        self.assertIsNotNone(child.returncode)
        self.assert_child_stopped()

    def test_live_eof_and_missing_eof(self):
        for drain in (True,False):
            self.pidfile.unlink(missing_ok=True)
            command = [sys.executable,str(ROOT/'examples/live-video/receive.py'),
                '--iq',str(self.iq),'--receiver',str(self.frontend(drain=drain)),
                '--ldpc-helper',str(self.helper),'--rate','4000000','--symbol-rate','1000000']
            result = subprocess.run(command,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0 if drain else 1,result.stderr)
            self.assertEqual(len(result.stdout),376)
            if drain:
                report = json.loads(result.stderr.decode().splitlines()[-1])
                self.assertIsNone(report['fec']['ldpc_checks_after'])
                self.assertTrue(report['fec']['helper']['drained'])
            else:
                self.assertIn(b'incomplete external helper',result.stderr)
            self.assert_child_stopped()

    def test_plan_and_invalid_helper_before_startup(self):
        result = subprocess.run([sys.executable,'-S',str(ROOT/'examples/live-video/receive.py'),
            '--args','fake','--freq','1','--ldpc-helper','missing','--plan'],
            capture_output=True,check=True)
        self.assertIn('--ldpc-helper',json.loads(result.stdout)['decoder'])
        args = live.parser().parse_args(['--args','fake','--freq','1','--ldpc-helper','missing'])
        with patch.object(subprocess,'Popen',side_effect=AssertionError('opened source')):
            with self.assertRaises(FileNotFoundError): live.run(args)
        with self.assertRaises(FileNotFoundError):
            decode.decode(self.iq,sample_rate=4e6,symbol_rate=1e6,
                          receiver=self.frontend(),ldpc_helper='missing',modcod=4)


if __name__ == '__main__':
    unittest.main()
