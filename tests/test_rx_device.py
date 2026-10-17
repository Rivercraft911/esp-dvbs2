"""Fake Soapy driver only. Never imports a real SDR binding or discovers hardware."""
import contextlib
import io
import os
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host'))
import demo_rx
from rx_device import SoapyRX


class DeviceTests(unittest.TestCase):
    def fake(self, device='b200'):
        args = demo_rx.arguments(['--device', device])
        radio = Mock()
        for key, value in [('SampleRate', args.sample_rate), ('Frequency', args.center),
                           ('Bandwidth', args.bandwidth), ('Gain', args.gain),
                           ('Antenna', args.antenna), ('GainMode', False)]:
            getattr(radio, 'get'+key).return_value = value
        radio.hasGainMode.return_value = True
        radio.activateStream.return_value = 0
        api = SimpleNamespace(Device=Mock(return_value=radio), SOAPY_SDR_RX=1,
                              SOAPY_SDR_CF32='CF32', SOAPY_SDR_TIMEOUT=-1,
                              SOAPY_SDR_END_ABRUPT=8, SOAPY_SDR_END_BURST=2,
                              errToStr=lambda code: 'OVERFLOW')
        return args, radio, api

    def test_live_defaults_are_pure_and_backend_specific(self):
        # A deliberately unusable binding catches any accidental device access in parsing.
        with patch.dict(sys.modules, {'SoapySDR': None}):
            for device in ('b200', 'e200'):
                a = demo_rx.arguments(['--device', device])
                self.assertEqual((a.sample_rate, a.symbol_rate, a.bandwidth, a.gain),
                                 (20_000_000, 10_000_000, 13_500_000, 0))
                self.assertEqual((a.format, a.scale, a.backend), ('f32', 1, 'soapy'))
            a = demo_rx.arguments(['--device', 'e200', '--backend', 'libiio'])
            self.assertEqual((a.format, a.scale), ('s16', 2048))
            self.assertEqual(demo_rx.arguments(['--e200']).backend, 'libiio')
            a = demo_rx.arguments(['--device', 'b200', '--symbol-rate', '20000000'])
            self.assertEqual((a.sample_rate, a.bandwidth), (40_000_000, 27_000_000))

    def test_invalid_device_settings_fail_before_access(self):
        cases = [['--device','b200','--backend','libiio'],
                 ['--device','b200','--format','s16'],
                 ['--device','b200','--sample-rate','80000000'],
                 ['--device','b200','--sample-rate','10000000'],
                 ['--device','b200','--bandwidth','1000000'],
                 ['--device','b200','--gain','nan'],
                 ['--device','b200','--unpaced'], ['--device','e200','--serial','123'],
                 ['--device','b200','--file','no.iq']]
        for argv in cases:
            with self.subTest(argv=argv), self.assertRaises(SystemExit), \
                    contextlib.redirect_stderr(io.StringIO()):
                demo_rx.arguments(argv)

    def test_configure_one_rx_with_readbacks(self):
        for device in ('b200', 'e200'):
            a, r, api = self.fake(device)
            with patch.dict(sys.modules, {'SoapySDR': api}):
                source = SoapyRX(a)
                meta = source.configure()
                expected = ({'driver':'uhd','type':'b200'} if device == 'b200' else
                            {'driver':'plutosdr','uri':a.uri})
                api.Device.assert_called_once_with(expected)
                r.setGainMode.assert_called_once_with(1, 0, False)
                r.setFrequency.assert_called_once_with(1, 0, 2420000000)
                r.setupStream.assert_called_once_with(1, 'CF32', [0],
                    {'WIRE':'sc16'} if device == 'b200' else {})
                self.assertEqual(meta['readback']['sample_rate'], a.sample_rate)
                source.close()
                r.closeStream.assert_called_once()
                r.deactivateStream.assert_not_called()

    def test_coerced_settings_rejected_and_device_released(self):
        for key, wrong in [('SampleRate', 30_720_000), ('Frequency', 2400000000),
                           ('Bandwidth', 1000000), ('Gain', 20), ('Antenna', 'TX/RX'),
                           ('GainMode', True)]:
            a, r, api = self.fake()
            getattr(r, 'get'+key).return_value = wrong
            with self.subTest(key=key), patch.dict(sys.modules, {'SoapySDR':api}):
                source = SoapyRX(a)
                with self.assertRaises(ValueError):
                    source.configure()
                source.close()
                self.assertIsNone(source.radio)
                r.setupStream.assert_not_called()

    def test_explicit_settings_and_serial_reach_driver(self):
        a, r, api = self.fake()
        a.serial, a.sample_rate, a.center, a.bandwidth, a.gain = 'unit123', 40_000_000, 2430000000, 27_000_000, 12
        r.getSampleRate.return_value = a.sample_rate
        r.getFrequency.return_value = a.center
        r.getBandwidth.return_value = a.bandwidth
        r.getGain.return_value = a.gain
        with patch.dict(sys.modules, {'SoapySDR':api}):
            source=SoapyRX(a)
            source.configure()
            api.Device.assert_called_once_with({'driver':'uhd','type':'b200','serial':'unit123'})
            r.setSampleRate.assert_called_once_with(1,0,40_000_000)
            r.setFrequency.assert_called_once_with(1,0,2430000000)
            r.setBandwidth.assert_called_once_with(1,0,27_000_000)
            r.setGain.assert_called_once_with(1,0,12)
            source.close()

    def test_short_read_timeout_overflow_and_cleanup(self):
        a, r, api = self.fake()
        responses = iter([(-1,0), (3,0), (-4,0)])
        def read(stream, buffers, count, timeoutUs):
            buffers[0][:3] = [1+2j, 3+4j, 5+6j]
            ret, flags = next(responses)
            return SimpleNamespace(ret=ret, flags=flags)
        r.readStream.side_effect = read
        with patch.dict(sys.modules, {'SoapySDR':api}):
            source = SoapyRX(a)
            source.configure()
            blocks = source.blocks(threading.Event())
            np.testing.assert_array_equal(np.frombuffer(next(blocks), '<c8'), [1+2j,3+4j,5+6j])
            with self.assertRaisesRegex(RuntimeError, 'OVERFLOW; continuity lost'):
                next(blocks)
            source.close()
            source.close()
            r.deactivateStream.assert_called_once()
            r.closeStream.assert_called_once()

    def test_timeout_and_abrupt_end_are_errors(self):
        for ret, flags in [(-1,0), (2,8), (2,2)]:
            a, r, api = self.fake()
            r.readStream.return_value = SimpleNamespace(ret=ret, flags=flags)
            with patch.dict(sys.modules, {'SoapySDR':api}), \
                    patch('rx_device.time.monotonic', side_effect=[0, 6]):
                source = SoapyRX(a)
                source.configure()
                with self.assertRaises(RuntimeError):
                    next(source.blocks(threading.Event()))
                source.close()

    def test_stop_does_not_start_stream(self):
        a, r, api = self.fake()
        stop = threading.Event()
        with patch.dict(sys.modules, {'SoapySDR':api}):
            source = SoapyRX(a)
            source.configure()
            stop.set()
            self.assertEqual(list(source.blocks(stop)), [])
            source.close()
            r.readStream.assert_not_called()
            r.activateStream.assert_not_called()

    def test_receiver_uses_device_bytes_and_surfaces_failure(self):
        for failure in (False, True):
            a = demo_rx.arguments(['--device','b200'])
            s = demo_rx.State(a)
            receiver = demo_rx.Receiver(a,s)
            raw = np.array([1+2j,3+4j,5+6j],dtype='<c8').tobytes()
            fake = Mock()
            fake.configure.return_value = {'readback':{'sample_rate':20_000_000}}
            def blocks(stop):
                yield raw
                if failure:
                    raise RuntimeError('SDR RX OVERFLOW; continuity lost')
            fake.blocks.side_effect = blocks
            read_fd,write_fd = os.pipe()
            with os.fdopen(read_fd,'rb') as received:
                sink = os.fdopen(write_fd,'wb')
                receiver.decoder = Mock(stdin=sink)
                receiver.decoder.wait.return_value = 0
                with patch('rx_device.SoapyRX',return_value=fake):
                    receiver.thread(receiver.input)
                    receiver.threads[0].join(timeout=2)
                    self.assertFalse(receiver.threads[0].is_alive())
                    sink.close()
                    self.assertEqual(received.read(),raw)
                self.assertEqual(s.data['iq_samples'],3)
                self.assertEqual(s.data['source'],'B200 / SOAPY')
                self.assertEqual(s.data['device_settings'],fake.configure.return_value)
                self.assertEqual(s.data['status'],'failed' if failure else 'ended')
                if failure:
                    self.assertIn('continuity lost',s.data['error'])
                fake.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()
