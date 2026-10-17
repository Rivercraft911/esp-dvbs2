"""SoapySDR RX source. Import and device access happen only on explicit live selection."""
import math
import time

import numpy as np


class SoapyRX:
    def __init__(self, args):
        self.args = args
        self.radio = self.stream = None
        self.active = False

    def configure(self):
        import SoapySDR
        self.api = SoapySDR
        a = self.args
        selector = ({"driver": "uhd", "type": "b200"} if a.device == "b200" else
                    {"driver": "plutosdr", "uri": a.uri})
        if a.serial:
            selector["serial"] = a.serial
        self.radio = r = SoapySDR.Device(selector)
        rx = SoapySDR.SOAPY_SDR_RX
        r.setSampleRate(rx, 0, a.sample_rate)
        r.setFrequency(rx, 0, a.center)
        r.setBandwidth(rx, 0, a.bandwidth)
        r.setAntenna(rx, 0, a.antenna)
        if r.hasGainMode(rx, 0):
            r.setGainMode(rx, 0, False)
        r.setGain(rx, 0, a.gain)
        actual = dict(sample_rate=r.getSampleRate(rx, 0), center=r.getFrequency(rx, 0),
                      bandwidth=r.getBandwidth(rx, 0), gain=r.getGain(rx, 0),
                      antenna=r.getAntenna(rx, 0))
        # Reject silent coercion. Allow clock rounding, RF filter tuning and gain steps.
        for key, rel, absolute in (("sample_rate", 1e-6, 1), ("center", 0, 1),
                                   ("bandwidth", .01, 1), ("gain", 0, .5)):
            if not math.isclose(actual[key], getattr(a, key), rel_tol=rel, abs_tol=absolute):
                raise ValueError(f"SDR {key}: requested {getattr(a, key)}, got {actual[key]}")
        if actual["antenna"] != a.antenna or (r.hasGainMode(rx, 0) and r.getGainMode(rx, 0)):
            raise ValueError("SDR antenna or manual gain readback differs")
        wire = {"WIRE": "sc16"} if a.device == "b200" else {}
        self.stream = r.setupStream(rx, SoapySDR.SOAPY_SDR_CF32, [0], wire)
        return dict(selector=selector, requested=dict(sample_rate=a.sample_rate, center=a.center,
                    bandwidth=a.bandwidth, gain=a.gain, antenna=a.antenna), readback=actual,
                    format="CF32", wire_args=wire)

    def blocks(self, stop):
        r, api = self.radio, self.api
        if stop.is_set():
            return
        code = r.activateStream(self.stream)
        if code:
            raise RuntimeError(f"SDR activateStream failed: {code}")
        self.active = True
        buf = np.empty(262144, dtype="<c8")
        last = time.monotonic()
        while not stop.is_set():
            result = r.readStream(self.stream, [buf], len(buf), timeoutUs=200000)
            if stop.is_set():
                return
            if result.ret in (0, api.SOAPY_SDR_TIMEOUT):
                if time.monotonic()-last > 5:
                    raise RuntimeError("SDR RX timed out for 5 seconds")
                continue
            if result.ret < 0:
                raise RuntimeError(f"SDR RX {api.errToStr(result.ret)}; continuity lost")
            if result.flags & (api.SOAPY_SDR_END_ABRUPT | api.SOAPY_SDR_END_BURST):
                raise RuntimeError("SDR RX ended unexpectedly; continuity lost")
            last = time.monotonic()
            yield buf[:result.ret].tobytes()

    def close(self):
        try:
            if self.stream is not None:
                try:
                    if self.active:
                        self.radio.deactivateStream(self.stream)
                finally:
                    self.radio.closeStream(self.stream)
        finally:
            self.stream = self.radio = None
            self.active = False
