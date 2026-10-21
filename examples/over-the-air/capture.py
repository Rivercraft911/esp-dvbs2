#!/usr/bin/env python3
"""Bounded, receive-only UHD worker for check.py; never discovers devices."""
import argparse
import json
import math
from pathlib import Path
import time


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def check_readback(actual, settings):
    for key in ("sample_rate", "master_clock_rate", "frequency", "gain", "bandwidth"):
        if not math.isfinite(actual[key]):
            raise ValueError(f"nonfinite UHD readback: {key}")
    for key in ("sample_rate", "master_clock_rate", "frequency", "bandwidth"):
        if abs(actual[key] - settings[key]) > 1:
            raise ValueError(f"UHD coerced {key}: requested {settings[key]}, got {actual[key]}")
    if not settings["occupied_hz"] <= actual["bandwidth"] <= 20e6:
        raise ValueError("actual bandwidth does not fit the signal / AD9363")
    if abs(actual["gain"] - settings["gain"]) > .5 or actual["antenna"] != settings["antenna"]:
        raise ValueError("UHD changed the manual gain or antenna")
    if not actual["lo_locked"]:
        raise ValueError("receive LO is not locked")


def capture(settings, out):
    # Load UHD only when starting a capture.
    report = {"complete": False, "samples": 0, "overflows": 0}
    streamer = None
    try:
        import numpy as np
        import uhd

        usrp = uhd.usrp.MultiUSRP(settings["args"])
        usrp.set_clock_source("internal")
        usrp.set_master_clock_rate(settings["master_clock_rate"])
        usrp.set_rx_rate(settings["sample_rate"], 0)
        usrp.set_rx_bandwidth(settings["bandwidth"], 0)
        usrp.set_rx_freq(uhd.types.TuneRequest(settings["frequency"]), 0)
        usrp.set_rx_agc(False, 0)
        usrp.set_rx_gain(settings["gain"], 0)
        usrp.set_rx_antenna(settings["antenna"], 0)
        deadline = time.monotonic() + 5
        while not usrp.get_rx_sensor("lo_locked", 0).to_bool():
            if time.monotonic() >= deadline:
                raise RuntimeError("receive LO failed to lock")
            time.sleep(.05)
        actual = dict(sample_rate=usrp.get_rx_rate(0),
                      master_clock_rate=usrp.get_master_clock_rate(),
                      bandwidth=usrp.get_rx_bandwidth(0), frequency=usrp.get_rx_freq(0),
                      gain=usrp.get_rx_gain(0), antenna=usrp.get_rx_antenna(0),
                      lo_locked=usrp.get_rx_sensor("lo_locked", 0).to_bool())
        check_readback(actual, settings)
        report.update(actual=actual, device=usrp.get_usrp_rx_info(0),
                      uhd_version=uhd.get_version_string())
        stream_args = uhd.usrp.StreamArgs("fc32", "sc16")
        stream_args.channels = [0]
        streamer = usrp.get_rx_stream(stream_args)
        buffer = np.empty((1, 262144), dtype=np.complex64)
        metadata = uhd.types.RXMetadata()
        command = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
        command.stream_now = True
        limit = int(settings["capture_limit_seconds"] * settings["sample_rate"])
        started = time.monotonic()
        with (out / "capture.cfile").open("xb", buffering=0) as sink:
            streamer.issue_stream_cmd(command)
            while not (out / "stop").exists():
                count = streamer.recv(buffer, metadata, 2.0)
                if metadata.error_code != uhd.types.RXMetadataErrorCode.none:
                    report["overflows"] += int(metadata.error_code == uhd.types.RXMetadataErrorCode.overflow)
                    raise RuntimeError("UHD receive error: " + metadata.strerror())
                if not count:
                    raise RuntimeError("UHD returned no samples")
                buffer[0, :count].astype("<c8", copy=False).tofile(sink)
                report["samples"] += count
                if not (out / "ready.json").exists():
                    report["first_sample_device_seconds"] = metadata.time_spec.get_real_secs()
                    write_json(out / "ready.json", report)
                if (report["samples"] >= limit or
                        time.monotonic() - started > settings["capture_limit_seconds"] + 5):
                    raise RuntimeError("capture safety limit reached before coordinator stop")
        report["complete"] = True
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        if streamer is not None:
            try:
                streamer.issue_stream_cmd(uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
            except Exception as exc:
                report.update(complete=False, stop_error=str(exc))
        write_json(out / "capture.json", report)
    return 0 if report["complete"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("settings", type=Path)
    parser.add_argument("--key", action="store_true", help="required coordinator authorization")
    args = parser.parse_args()
    if not args.key:
        parser.error("worker requires explicit --key; use check.py")
    settings = json.loads(args.settings.read_text())
    return capture(settings, args.settings.parent)


if __name__ == "__main__":
    raise SystemExit(main())
