"""File-only receiver checks. Optional integration: RX_INTEGRATION=1 with LEANDVB/FFMPEG."""
import argparse
import io
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host'))
import demo_rx
import dvbs2
import iq
import rxbench
import rx_fixture
import rx_rate
import sdr


class ReceiverTests(unittest.TestCase):
    def state(self):
        return demo_rx.State(argparse.Namespace(file=Path('test.iq'), unpaced=False,
            sample_rate=20_000_000, symbol_rate=10_000_000, center=2420e6))

    def test_status_and_stale_lock(self):
        s = self.state()
        s.data['status'] = 'running'
        s.status_line('_FRAMELOCK 1\n')
        s.status_line('S2FEC modcod=13 short=0 pilots=1 ldpc_flips=8 ldpc_checks_before=9 ldpc_checks_after=0 bch_errors=0\n')
        s.status_line('S2FEC modcod=13 short=0 pilots=1 ldpc_flips=200 ldpc_checks_before=90 ldpc_checks_after=5 bch_errors=-1\n')
        s.status_line('_MER 15.2\nBad bbframe\n')
        d = s.snapshot()
        self.assertEqual((d['frames_ok'], d['frames_failed'], d['modcod']), (1, 1, 13))
        self.assertEqual((d['mer_db'], d['bbheader_failed']), (15.2, 1))
        self.assertTrue(d['healthy'])
        s.fec_at -= 3
        self.assertFalse(s.snapshot()['healthy'])
        s.status_line('FRAMELOCK 0\n')
        self.assertFalse(s.snapshot()['frame_lock'])

    def test_bitrate_uses_wall_clock_and_decays(self):
        s = self.state()
        s.data['ts_bytes'] = 1_000_000
        s.ts_history.append((time.monotonic()-2, 0))
        s.ts_history.append((time.monotonic()-1, 1_000_000))
        self.assertAlmostEqual(s.snapshot()['received_mbps'], 4, places=2)
        s.ts_history[0] = (time.monotonic()-20, 0)
        self.assertAlmostEqual(s.snapshot()['received_mbps'], .4, places=2)

    def test_fft_frequency_axis_and_full_scale(self):
        n = np.arange(1024)
        z = np.exp(2j*np.pi*128*n/1024).astype('<c8')
        db = demo_rx.spectrum_db(z.tobytes(), 'f32')
        self.assertEqual(np.argmax(db), 512+128)
        self.assertAlmostEqual(max(db), 0, places=1)
        raw = np.round(np.column_stack([z.real,z.imag])*2047).astype('<i2').tobytes()
        self.assertAlmostEqual(max(demo_rx.spectrum_db(raw, 's16', 2048)), 0, places=1)

    def test_noise_units(self):
        z = np.ones(200000, dtype=np.complex64)
        y = rxbench.add_noise(z, 20e6, 10e6, 10, 2026)
        # Es/N0=10 linear, Fs/Rs=2 => complex sample noise variance 0.2.
        self.assertAlmostEqual(float(np.mean(abs(y-z)**2)), .2, places=2)
        np.testing.assert_array_equal(rxbench.add_noise(z,20e6,10e6,None,0),z)

    def test_jpeg_fragmentation(self):
        frames = [b'\xff\xd8hello\xff\xd9', b'\xff\xd8world\xff\xd9']
        class Fragmented(io.BytesIO):
            def read(self, n=-1):
                return super().read(3)
        self.assertEqual(list(demo_rx.jpeg_frames(Fragmented(b'junk'+b''.join(frames)))), frames)

    def test_circular_shaping_matches_middle_period(self):
        p = np.random.default_rng(2).integers(0,8,100,dtype=np.uint8)
        padded = np.r_[p[-6:],p,p[:6]]
        got = iq.shape(padded, 1e6, 2e6)[24:224]
        full = iq.shape(np.tile(p,3),1e6,2e6)[212:412]
        np.testing.assert_allclose(got,full,atol=1e-7)

    def test_pipe_chunks_and_idle_stop(self):
        r,w = os.pipe()
        stop = threading.Event()
        with os.fdopen(r,'rb') as src:
            os.write(w,b'1234567')
            os.close(w)
            self.assertEqual(list(demo_rx.read_blocks(src,4,stop)), [b'1234',b'567'])
        r,w = os.pipe()
        with os.fdopen(r,'rb') as src:
            timer=threading.Timer(.05,stop.set);timer.start()
            self.assertEqual(list(demo_rx.read_blocks(src,4,stop)), [])
            timer.join()
        os.close(w)

    def test_reject_serial_as_telemetry(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            demo_rx.arguments(['--stdin','--sample-rate','20000000','--symbol-rate','10000000',
                               '--telemetry','/dev/null'])


    def test_telemetry_regular_file_and_invalid_records(self):
        s = self.state()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'tx.jsonl'
            path.write_text('bad json\n{"fill_slack_us": NaN}\n{"fill_slack_us": 24.5, "encoder_load_pct": 42}\n')
            rx=demo_rx.Receiver(argparse.Namespace(telemetry=path),s)
            t=threading.Thread(target=rx.telemetry)
            t.start()
            deadline=time.monotonic()+1
            while s.data['tx'] is None and time.monotonic()<deadline: time.sleep(.01)
            rx.stop.set();t.join(timeout=1)
            self.assertEqual(s.snapshot()['tx'],{'fill_slack_us':24.5,'encoder_load_pct':42})
            self.assertLess(s.snapshot()['tx_age_s'],1)

    def test_rate_command_and_truncated_stream(self):
        radio=sdr.SDR(sdr.Settings())
        cmd=rx_rate.stream_command(radio)
        self.assertEqual(cmd[-3:],['cf-ad9361-lpc','voltage0','voltage1'])
        self.assertNotIn('-s',cmd)
        r,w=os.pipe()
        os.write(w,b'12345');os.close(w)
        with os.fdopen(r,'rb') as source: report=rx_rate.drain(source,1000,1)
        self.assertTrue(report['partial_sample'])
        self.assertTrue(report['early_eof'])
        self.assertFalse(report['delivery_pass'])
        self.assertFalse(report['continuity_proven'])


@unittest.skipUnless(os.environ.get('RX_INTEGRATION'), 'set RX_INTEGRATION=1 for reference IQ integration')
class IntegrationTests(unittest.TestCase):
    def test_reference_video_through_decoder_and_dashboard(self):
        root=Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)/'fixture'
            fixture,meta=rx_fixture.make_fixture(directory,seconds=1)
            # Allocate an unused loopback port without connecting to any device.
            with socket.socket() as sock:
                sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            cmd=[sys.executable,str(root/'host/demo_rx.py'),'--file',str(fixture),'--format','f32',
                 '--sample-rate',str(meta['sample_rate']),'--symbol-rate',str(meta['symbol_rate']),
                 '--receiver',os.environ['LEANDVB'],'--loop','--no-open','--port',str(port),
                 '--seconds','8','--ts-out',str(Path(tmp)/'received.ts')]
            with subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) as proc:
                self.assertEqual(proc.stdout.readline().strip(),f'http://127.0.0.1:{port}')
                state=None
                for _ in range(60):
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/state') as response:
                        state=json.load(response)
                    if state['video_frames']>3 and state['source_loops']>1: break
                    time.sleep(.1)
                self.assertGreater(state['video_frames'],3)
                self.assertTrue(state['healthy'],state)
                self.assertLess(state['behind_s'],.3)
                self.assertEqual(state['frames_failed'],0)
                self.assertEqual(len(state['spectrum']),1024)
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/frame.jpg') as response:
                    jpeg=response.read()
                self.assertTrue(jpeg.startswith(b'\xff\xd8'))
                self.assertTrue(jpeg.endswith(b'\xff\xd9'))
                out,err=proc.communicate(timeout=15)
                self.assertEqual(proc.returncode,0,err)
                final=json.loads(out.strip())
                self.assertGreater(final['video_frames'],10)
            received=(Path(tmp)/'received.ts').read_bytes()
            self.assertEqual(len(received)%188,0)
            self.assertTrue(all(received[n]==0x47 for n in range(0,len(received),188)))
            # Ignore acquisition and a leading PAT. Several actual video packets must match.
            original=(directory/'video.ts').read_bytes()
            self.assertIn(original[188*100:188*120],received)


if __name__ == '__main__':
    unittest.main()
