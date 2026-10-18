#!/usr/bin/env python3
"""Receive-only Pluto-firmware worker. Only --key may construct an IIO context."""
import argparse
import json
import math
from pathlib import Path
import time

from capture import write_json


def number(value):
    # AD9361 hardwaregain reads back as e.g. '30.000000 dB'.
    result = float(value.split()[0])
    if not math.isfinite(result):
        raise ValueError("nonfinite IIO attribute")
    return result


class Radio:
    def __init__(self, context):
        self.context = context
        self.phy = context.find_device("ad9361-phy")
        self.dds = context.find_device("cf-ad9361-dds-core-lpc")
        self.rx = context.find_device("cf-ad9361-lpc")
        if any(device is None for device in (self.phy, self.dds, self.rx)):
            raise ValueError("need Pluto-firmware AD9361 PHY, DDS and RX devices")
        self.tx_lo = self.channel(self.phy, "altvoltage1", True)
        self.tx_gain = [c for c in self.phy.channels if c.output and c.id.startswith("voltage")]
        self.tones = [c for c in self.dds.channels if c.output and c.id.startswith("altvoltage")]
        if not self.tx_gain or len(self.tones) < 4:
            raise ValueError("cannot identify every TX gain / DDS tone inhibit")

    @staticmethod
    def channel(device, name, output=False):
        channel = device.find_channel(name, output)
        if channel is None:
            raise ValueError(f"missing IIO channel {device.name}/{name}")
        return channel

    def inhibit_tx(self):
        # Set LO powerdown first. Never restore previous TX settings, even on failure.
        self.tx_lo.attrs["powerdown"].value = "1"
        for channel in self.tx_gain:
            channel.attrs["hardwaregain"].value = "-89.75"
        for channel in self.tones:
            channel.attrs["raw"].value = "0"
        return self.verify_tx()

    def verify_tx(self):
        state = dict(lo_powerdown=number(self.tx_lo.attrs["powerdown"].value),
                     hardwaregain={c.id: number(c.attrs["hardwaregain"].value) for c in self.tx_gain},
                     dds_raw={c.id: number(c.attrs["raw"].value) for c in self.tones})
        if (state["lo_powerdown"] != 1 or
                any(v != -89.75 for v in state["hardwaregain"].values()) or
                any(v != 0 for v in state["dds_raw"].values())):
            raise ValueError(f"TX inhibit readback failed; refusing capture: {state}")
        return state

    def configure(self, settings):
        self.inhibit_tx()
        lo = self.channel(self.phy, "altvoltage0", True)
        rx = self.channel(self.phy, "voltage0")
        lo.attrs["frequency"].value = str(int(settings["frequency"]))
        for attribute, key in (("sampling_frequency", "sample_rate"), ("rf_bandwidth", "bandwidth")):
            rx.attrs[attribute].value = str(int(settings[key]))
        rx.attrs["gain_control_mode"].value = "manual"
        rx.attrs["hardwaregain"].value = str(settings["gain"])
        actual = dict(frequency=number(lo.attrs["frequency"].value),
                      sample_rate=number(rx.attrs["sampling_frequency"].value),
                      bandwidth=number(rx.attrs["rf_bandwidth"].value),
                      gain=number(rx.attrs["hardwaregain"].value),
                      gain_control_mode=rx.attrs["gain_control_mode"].value)
        for key in ("frequency", "sample_rate", "bandwidth", "gain"):
            if abs(actual[key] - settings[key]) > (.5 if key == "gain" else 1):
                raise ValueError(f"IIO coerced {key}: {actual[key]} (requested {settings[key]})")
        if actual["gain_control_mode"] != "manual" or actual["bandwidth"] < settings["occupied_hz"]:
            raise ValueError("IIO manual gain / occupied bandwidth readback failed")
        # RX reconfiguration can calibrate the RFIC: reapply and verify all inhibits.
        tx = self.inhibit_tx()
        for channel in self.rx.channels:
            if channel.scan_element:
                channel.enabled = False
        for index in (0, 1):
            channel = self.channel(self.rx, f"voltage{index}")
            fmt = channel.data_format
            if (not channel.scan_element or channel.index != index or fmt.length != 16 or
                    fmt.bits != 12 or fmt.shift != 0 or not fmt.is_signed or fmt.is_be or
                    fmt.repeat != 1):
                raise ValueError("need little-endian signed 12-in-16 interleaved AD9361 I/Q")
            channel.enabled = True
        if self.rx.sample_size != 4:
            raise ValueError("RX scan must contain exactly one int16 I/Q pair")
        self.rx.set_kernel_buffers_count(1 if settings["burst_samples"] else 4)
        return actual, tx


def capture(settings, out):
    report = dict(backend="libiio", complete=False, samples=0, overflows=None,
                  continuity="No sample timestamps or hardware overflow counter exposed by this API",
                  capture_mode="burst" if settings["burst_samples"] else "stream")
    buffer = radio = None
    try:
        import numpy as np
        import iio

        context = iio.Context(settings["uri"])
        # A burst's USB/network transfer can greatly outlast its native RF window.
        context.set_timeout(30000 if settings["burst_samples"] else 5000)
        radio = Radio(context)
        actual, tx = radio.configure(settings)
        report.update(actual=actual, tx_inhibit=tx, device=dict(description=context.description,
                      attrs=context.attrs), libiio_version=iio.version)
        burst = settings["burst_samples"]
        count = burst or 262144
        deadline = time.monotonic() + settings["capture_limit_seconds"]
        if burst:
            # Do not allocate/queue the device buffer until after the leading CW ID.
            write_json(out / "ready.json", dict(report, armed=True))
            while not (out / "trigger").exists():
                if (out / "stop").exists() or time.monotonic() > deadline:
                    raise RuntimeError("burst cancelled / trigger deadline exceeded")
                time.sleep(.02)
        report["tx_before_buffer"] = radio.verify_tx()
        report["buffer_started_monotonic"] = time.monotonic()
        buffer = iio.Buffer(radio.rx, count, False)  # RX device only; never a TX buffer.
        if buffer.step != 4:
            raise ValueError("unexpected RX buffer stride")
        limit = int(settings["capture_limit_seconds"] * settings["sample_rate"])
        with (out / "capture.cfile").open("xb") as sink:
            while not (out / "stop").exists():
                buffer.refill()
                report["buffer_filled_monotonic"] = time.monotonic()
                raw = buffer.read()
                if len(raw) != count * 4:
                    raise RuntimeError("short IIO device buffer")
                values = np.frombuffer(raw, dtype="<i2").reshape(-1, 2).astype(np.float32) / 2048
                (values[:, 0] + 1j * values[:, 1]).astype("<c8").tofile(sink)
                sink.flush()
                report["samples"] += count
                if burst:
                    break  # Exactly one refill of one contiguous device buffer.
                if not (out / "ready.json").exists():
                    write_json(out / "ready.json", report)
                if report["samples"] >= limit or time.monotonic() > deadline:
                    raise RuntimeError("capture safety limit reached before coordinator stop")
        if report["samples"] < (burst or settings["pre_seconds"] * settings["sample_rate"]):
            raise RuntimeError("insufficient IIO samples")
        report["tx_after_capture"] = radio.verify_tx()
        report["complete"] = True
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        if buffer is not None:
            try:
                buffer.cancel()
            except Exception as exc:
                report.update(complete=False, stop_error=str(exc))
            del buffer
        if radio is not None:
            try:
                # RX setup or capture may have failed mid-calibration. Leave all
                # inhibits applied rather than relying on the initial snapshot.
                report["tx_final"] = radio.inhibit_tx()
            except Exception as exc:
                report.update(complete=False, tx_inhibit_error=str(exc))
        write_json(out / "capture.json", report)
    return 0 if report["complete"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("settings", type=Path)
    parser.add_argument("--key", action="store_true")
    args = parser.parse_args()
    if not args.key:
        parser.error("worker requires explicit --key; use check.py")
    settings = json.loads(args.settings.read_text())
    # Also validate direct worker invocations before importing iio.
    uri = settings.get("uri", "")
    if (not isinstance(uri, str) or not uri.startswith(("ip:", "usb:")) or
            not uri.split(":", 1)[1].strip() or settings.get("backend") != "libiio"):
        parser.error("need explicit libiio settings and URI")
    return capture(settings, args.settings.parent)


if __name__ == "__main__":
    raise SystemExit(main())
