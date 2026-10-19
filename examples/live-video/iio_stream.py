#!/usr/bin/env python3
"""Receive-only Pluto IQ streaming/probe worker; explicit URI required, no discovery."""
import argparse
import json
import math
import os
from pathlib import Path
import queue
import select
import signal
import sys
import threading
import time

# Share the OTA worker's fail-closed implementation, including all TX channels,
# calibration re-inhibit, RX format validation and final inhibit. Do not fork it.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "over-the-air"))
from capture_iio import Radio
from capture import write_json


def validate(a):
    if (not a.uri or not a.uri.startswith(("ip:", "usb:")) or
            not a.uri.split(":", 1)[1].strip()):
        raise ValueError("libiio needs explicit ip:HOST or usb:BUS.ADDR URI (no discovery)")
    if a.freq is None or not math.isfinite(a.freq) or a.freq <= 0:
        raise ValueError("libiio requires a positive finite --freq in Hz")
    if (not math.isfinite(a.rate) or not 2.5e6 <= a.rate <= 61.44e6 or
            not math.isfinite(a.symbol_rate) or a.symbol_rate <= 0 or
            a.rate < 1.35 * a.symbol_rate):
        raise ValueError("libiio rate must cover occupied bandwidth and be 2.5..61.44 MS/s")
    if a.bandwidth is None:
        a.bandwidth = round(1.35 * a.symbol_rate * 1.15)
    if not math.isfinite(a.bandwidth) or not 1.35 * a.symbol_rate <= a.bandwidth <= 56e6:
        raise ValueError("libiio bandwidth must cover signal and be <=56 MHz")
    if not math.isfinite(a.gain) or not 0 <= a.gain <= 76:
        raise ValueError("libiio manual gain must be 0..76 dB")
    if not math.isfinite(a.seconds) or not 1 <= a.seconds <= 3600:
        raise ValueError("libiio duration must be 1..3600 seconds")
    if not 65536 <= a.buffer_samples <= 4194304 or a.upsample not in (1, 2):
        raise ValueError("need 65536..4194304 buffer samples and upsample 1 or 2")
    if not getattr(a, "probe", False) and a.rate * a.upsample < 1.5 * a.symbol_rate:
        raise ValueError("LeanDVB needs at least 1.5 samples/symbol (see receive.py)")


class Interpolator:
    """Integer polyphase FIR with history AND lookahead across refill boundaries."""
    def __init__(self, factor):
        self.factor, self.pending, self.left = factor, None, None

    def push(self, raw):
        import numpy as np
        from scipy.signal import resample_poly
        right = None if raw is None else np.frombuffer(raw, dtype="<i2").reshape(-1, 2).astype(np.float32)
        result = b""
        if self.pending is not None:
            left = self.left if self.left is not None else self.pending[:0]
            future = right[:32] if right is not None else self.pending[:0]
            block = np.concatenate((left, self.pending, future))
            values = resample_poly(block, self.factor, 1, axis=0)
            values = values[len(left)*self.factor:(len(left)+len(self.pending))*self.factor]
            result = np.clip(np.rint(values), -32768, 32767).astype("<i2").tobytes()
            self.left = self.pending[-32:]
        self.pending = right
        return result


class Timing:
    """Host symptoms only: none of these measurements is a hardware loss counter."""
    def __init__(self, rate, count, started):
        self.duration, self.started = count / rate, started
        self.rows = dict(refills=0, refill_seconds_max=0, slow_refills=0,
                         host_lag_seconds_max=0, queue_depth_max=0, queue_age_seconds_max=0)

    def refill(self, before, after):
        r = self.rows
        r["refills"] += 1
        r["refill_seconds_max"] = max(r["refill_seconds_max"], after-before)
        if r["refills"] == 1:
            self.first = after  # stream start-up latency is not sample loss
            return
        r["slow_refills"] += int(after-before > 1.5*self.duration)  # diagnostic only
        lag = max(0, after-self.first-(r["refills"]-1)*self.duration)
        r["host_lag_seconds_max"] = max(r["host_lag_seconds_max"], lag)

    def passed(self):
        # Samples drop only once the host trails the device's four kernel buffers; fail at half that.
        return self.rows["host_lag_seconds_max"] <= 2*self.duration


def write_iq(fd, raw, failed):
    remaining = memoryview(raw)
    deadline = time.monotonic() + 5
    while remaining:
        if failed or time.monotonic() >= deadline:
            raise RuntimeError("IQ consumer stalled or capture failed")
        if select.select([], [fd], [], .05)[1]:
            try:
                size = os.write(fd, remaining)
            except BlockingIOError:
                continue
            if size <= 0:
                raise RuntimeError("short IQ pipe write")
            remaining = remaining[size:]


def stream(a, iio=None):
    """Only this function imports iio/opens a context. Injectable for host tests."""
    report = dict(backend="libiio", complete=False, samples=0, delivered_samples=0,
                  host_timing_passed=False,
                  overflows=None, continuity="unknown: no hardware counter or timestamps; payload proof required",
                  uri=a.uri, requested_rate=a.rate, requested_seconds=a.seconds,
                  buffer_samples=a.buffer_samples, kernel_buffers=4, host_queue_buffers=4,
                  capture_format="sc16_le", upsample=a.upsample, probe=a.probe)
    radio = buffer = reader = fd = timing = None
    cancel = threading.Event()
    failed = []
    chunks = queue.Queue(4)
    finished = threading.Event()
    try:
        if a.upsample != 1:
            # Fail missing dependencies before opening a device.
            import numpy, scipy.signal  # noqa: F401
        if iio is None:
            import iio
        context = iio.Context(a.uri)
        context.set_timeout(5000)
        radio = Radio(context)
        settings = dict(frequency=a.freq, sample_rate=a.rate, bandwidth=a.bandwidth,
                        gain=a.gain, occupied_hz=1.35*a.symbol_rate, burst_samples=0)
        actual, tx = radio.configure(settings)
        report.update(actual=actual, tx_inhibit=tx, device=dict(description=context.description,
                      attrs=context.attrs), libiio_version=iio.version)
        if not a.probe:
            fd = os.open(a.output, os.O_WRONLY | os.O_NONBLOCK)
        report["tx_before_buffer"] = radio.verify_tx()
        started = time.monotonic()
        timing = Timing(a.rate, a.buffer_samples, started)
        report.update(buffer_duration_seconds=timing.duration, kernel_buffer_seconds=4*timing.duration,
                      host_queue_seconds=4*timing.duration)
        buffer = iio.Buffer(radio.rx, a.buffer_samples, False)
        if buffer.step != 4:
            raise ValueError("unexpected RX buffer stride")
        limit = math.ceil(a.seconds*a.rate/a.buffer_samples)
        deadline = started + a.seconds + 10

        def capture():
            try:
                for _ in range(limit):
                    if cancel.is_set() or (a.stop_file and a.stop_file.exists()):
                        break
                    before = time.monotonic()
                    buffer.refill()
                    after = time.monotonic()
                    timing.refill(before, after)
                    if not timing.passed():
                        raise RuntimeError("suspected sample loss from refill time / cumulative host lag")
                    raw = buffer.read()
                    if len(raw) != a.buffer_samples*4:
                        raise RuntimeError("short IIO device buffer")
                    report["samples"] += a.buffer_samples
                    if after > deadline:
                        raise RuntimeError("capture wall deadline exceeded")
                    try:
                        chunks.put_nowait((after, raw))
                    except queue.Full:
                        raise RuntimeError("host IQ queue full; consumer cannot sustain sample rate") from None
                    timing.rows["queue_depth_max"] = max(timing.rows["queue_depth_max"], chunks.qsize())
            except Exception as exc:
                if not cancel.is_set():
                    failed.append(str(exc))
            finally:
                finished.set()

        reader = threading.Thread(target=capture, daemon=True)
        reader.start()
        interpolator = Interpolator(a.upsample) if a.upsample > 1 else None
        while not finished.is_set() or not chunks.empty():
            if failed:
                raise RuntimeError(failed[0])
            try:
                arrived, raw = chunks.get(timeout=.05)
            except queue.Empty:
                continue
            timing.rows["queue_age_seconds_max"] = max(timing.rows["queue_age_seconds_max"], time.monotonic()-arrived)
            data = interpolator.push(raw) if interpolator else raw
            if fd is not None:
                write_iq(fd, data, failed)
            report["delivered_samples"] += len(data)//4
            if a.ready and report["delivered_samples"] >= a.rate*a.upsample and not a.ready.exists():
                write_json(a.ready, dict(ready=True, tx_before_buffer=report["tx_before_buffer"],
                                       samples=report["delivered_samples"], pid=os.getpid()))
        if failed:
            raise RuntimeError(failed[0])
        if interpolator:
            data = interpolator.push(None)
            if fd is not None:
                write_iq(fd, data, failed)
            report["delivered_samples"] += len(data)//4
        elapsed = time.monotonic()-started
        report.update(capture_wall_seconds=elapsed, sample_seconds=report["samples"]/a.rate,
                      host_timing_passed=timing.passed())
        if report["samples"] < a.rate or not timing.passed():
            raise RuntimeError("insufficient IQ or suspected sample loss from host timing; see report")
        if fd is not None:
            write_iq(fd, bytes(4096*4), failed)  # finite EOF lookahead, outside captured samples
        report["complete"] = True
    except (Exception, KeyboardInterrupt) as exc:
        report.update(complete=False, error=str(exc) or type(exc).__name__)
    finally:
        cancel.set()
        if buffer is not None:
            try:
                buffer.cancel()
            except Exception as exc:
                report.update(complete=False, cancel_error=str(exc))
        if reader is not None:
            reader.join(timeout=6)
            if reader.is_alive():
                report.update(complete=False, thread_error="IIO refill did not cancel")
        if fd is not None:
            os.close(fd)
        if radio is not None:
            try:
                report["tx_after_capture"] = radio.verify_tx()
            except Exception as exc:
                report.update(complete=False, tx_readback_error=str(exc))
            try:
                report["tx_final"] = radio.inhibit_tx()  # never restore TX, including after failure
            except Exception as exc:
                report.update(complete=False, tx_inhibit_error=str(exc))
        if timing:
            report.update(timing.rows)
            report.setdefault("capture_wall_seconds", time.monotonic()-timing.started)
            report.setdefault("sample_seconds", report["samples"]/a.rate)
        if a.report:
            write_json(a.report, report)
        print(json.dumps(report, allow_nan=False), file=sys.stderr)
    return 0 if report["complete"] else 1


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--uri", required=True)
    p.add_argument("--freq", type=float, required=True)
    p.add_argument("--rate", type=float, required=True)
    p.add_argument("--symbol-rate", type=float, default=5e6)
    p.add_argument("--bandwidth", type=float)
    p.add_argument("--gain", type=float, default=0)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--buffer-samples", type=int, default=1048576)
    p.add_argument("--upsample", type=int, choices=(1, 2), default=1)
    p.add_argument("--probe", action="store_true", help="discard IQ; receive only; never accesses S3")
    p.add_argument("--output", help="existing FIFO for IQ; required without --probe")
    p.add_argument("--report", type=Path)
    p.add_argument("--ready", type=Path)
    p.add_argument("--stop-file", type=Path)
    p.add_argument("--plan", action="store_true")
    return p


def main():
    p = parser()
    a = p.parse_args()
    try:
        validate(a)
        if not a.probe and not a.output:
            raise ValueError("need --output FIFO or --probe")
        if any(path and path.exists() for path in (a.report, a.ready, a.stop_file)):
            raise ValueError("report/ready/stop paths must be new")
    except ValueError as exc:
        p.error(str(exc))
    if a.plan:
        print(json.dumps(vars(a), default=str, indent=2))
        return 0
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    return stream(a)


if __name__ == "__main__":
    raise SystemExit(main())
