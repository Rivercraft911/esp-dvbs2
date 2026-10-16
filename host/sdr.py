#!/usr/bin/env python3
"""Configure E200 RX, verify read-backs, then capture one int16 I/Q buffer.

Uses libiio 0.25 command-line tools. No E200 transmit stream is opened. The
transmit LO is powered down before configuring RX and checked again at capture.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation
import datetime as dt
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


@dataclass(frozen=True)
class Settings:
    uri: str = "ip:192.168.1.10"
    lo_hz: int = 2420000000
    rate_hz: int = 4000000
    bandwidth_hz: int = 2000000
    gain_db: float = 0.0
    port: str = "A_BALANCED"

    def __post_init__(self):
        if any(not isinstance(v, int) or v <= 0 for v in
               (self.lo_hz, self.rate_hz, self.bandwidth_hz)):
            raise ValueError("LO, sample rate and bandwidth must be positive integer Hz")
        if not math.isfinite(self.gain_db) or not self.uri.strip() or not self.port.strip():
            raise ValueError("gain must be finite; URI and RF port must not be empty")

    def attributes(self) -> list[tuple[str, str, str, str]]:
        # Direction is explicit because the AD936x has overlapping channel names.
        return [("-o", "altvoltage1", "powerdown", "1"),
                ("-o", "altvoltage0", "powerdown", "0"),
                ("-i", "voltage0", "gain_control_mode", "manual"),
                ("-i", "voltage0", "hardwaregain", str(self.gain_db)),
                ("-i", "voltage0", "rf_port_select", self.port),
                ("-o", "altvoltage0", "frequency", str(self.lo_hz)),
                ("-i", "voltage0", "sampling_frequency", str(self.rate_hz)),
                ("-i", "voltage0", "rf_bandwidth", str(self.bandwidth_hz))]


class SDR:
    def __init__(self, settings: Settings, libiio: pathlib.Path | None = None,
                 timeout: float = 20):
        self.settings = settings
        self.timeout = timeout
        self.env = os.environ.copy()
        self.bin = libiio.resolve() / "bin" if libiio else None
        if libiio:
            for name in ("DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH"):
                self.env[name] = str(libiio.resolve() / "lib") + (
                    os.pathsep + self.env[name] if self.env.get(name) else "")
        self.metadata = None

    def tool(self, name: str) -> str:
        return str(self.bin / name) if self.bin else name

    def attr_command(self, attr, value=None) -> list[str]:
        direction, channel, name, _ = attr
        cmd = [self.tool("iio_attr"), "-u", self.settings.uri, direction, "-c",
               "ad9361-phy", channel, name]
        return cmd + ([value] if value is not None else [])

    def read_command(self, samples: int) -> list[str]:
        if not isinstance(samples, int) or not 0 < samples <= 2**31 - 1:
            raise ValueError("samples must be an integer in 1..2147483647")
        return [self.tool("iio_readdev"), "-u", self.settings.uri, "-T",
                str(int(self.timeout * 1000)), "-b", str(samples), "-s", str(samples),
                "cf-ad9361-lpc", "voltage0", "voltage1"]

    def command(self, cmd) -> str:
        result = subprocess.run(cmd, env=self.env, capture_output=True, text=True,
                                timeout=self.timeout, check=True)
        return result.stdout.strip()

    def verify(self) -> dict:
        readback = {}
        for attr in self.settings.attributes():
            value = self.command(self.attr_command(attr))
            expected = attr[3]
            numeric = value.removesuffix(" dB") if attr[2] == "hardwaregain" else value
            try:
                matches = Decimal(numeric) == Decimal(expected)
            except InvalidOperation:
                matches = value == expected
            key = f"{attr[0][1:]}/{attr[1]}/{attr[2]}"
            readback[key] = value
            if not matches:
                raise ValueError(f"SDR read-back {key}: requested {expected!r}, got {value!r}")
        return readback

    def scan_format(self) -> str:
        text = self.command([self.tool("iio_attr"), "-u", self.settings.uri,
                             "-s", "-c", "cf-ad9361-lpc"])
        for channel, index in (("voltage0", 0), ("voltage1", 1)):
            pattern = rf"channel '{channel}'[^\n]*\(input, index: {index}, format: le:S12/16>>0\)"
            if len(re.findall(pattern, text)) != 1:
                raise ValueError(f"unqualified SDR scan format for {channel}: {text}")
        return text

    def configure(self) -> dict:
        self.metadata = None
        # The tools' --version path does not create an IIO context.
        versions = {name: self.command([self.tool(name), "--version"])
                    for name in ("iio_attr", "iio_readdev")}
        for attr in self.settings.attributes():
            self.command(self.attr_command(attr, attr[3]))
            if attr[1] == "altvoltage1":
                if self.command(self.attr_command(attr)) != "1":
                    raise ValueError("E200 transmit LO did not power down")
        self.metadata = {"requested": asdict(self.settings), "readback": self.verify(),
                         "scan_format": self.scan_format(), "libiio_version": versions,
                         "configured_at": now()}
        return self.metadata

    def capture(self, path: pathlib.Path, samples: int, before_capture=None) -> dict:
        cmd = self.read_command(samples)
        if self.metadata is None:
            raise ValueError("configure and verify the SDR before capturing")
        path = pathlib.Path(path)
        sidecar = path.with_suffix(path.suffix + ".json")
        if path.exists() or sidecar.exists():
            raise FileExistsError(f"refusing to overwrite capture or sidecar: {path}")
        metadata = {**self.metadata, "capture": str(path), "samples": samples,
                    "format": "int16 I/Q LE", "adc_full_scale": 2048,
                    "buffer_samples": samples, "command": cmd, "host_time": now(),
                    "status": "failed"}
        # Reserve the sidecar as well as the sample file; keep failures for diagnosis.
        with sidecar.open("x") as record:
            try:
                metadata["readback"] = self.verify()
                metadata["scan_format"] = self.scan_format()
                if before_capture:
                    before_capture()
                metadata["capture_started_at"] = now()
                with path.open("xb") as sink:
                    result = subprocess.run(cmd, env=self.env, stdout=sink, stderr=subprocess.PIPE,
                                            timeout=self.timeout + samples / self.settings.rate_hz,
                                            check=False)
                metadata["stderr"] = result.stderr.decode(errors="replace")
                metadata["returncode"] = result.returncode
                metadata["bytes"] = path.stat().st_size
                if result.returncode or metadata["bytes"] != 4 * samples:
                    raise RuntimeError(f"SDR capture failed: exit {result.returncode}, "
                                       f"{metadata['bytes']} bytes, expected {4 * samples}; "
                                       f"{metadata['stderr']}")
                with path.open("rb") as source:
                    metadata["sha256"] = hashlib.file_digest(source, "sha256").hexdigest()
                metadata["status"] = "ok"
                return metadata
            except BaseException as exc:
                metadata["error"] = str(exc)
                raise
            finally:
                metadata["capture_finished_at"] = now()
                json.dump(metadata, record, indent=2, allow_nan=False)
                record.write("\n")


def add_arguments(parser):
    parser.add_argument("--uri", default="ip:192.168.1.10")
    parser.add_argument("--libiio", type=pathlib.Path, help="libiio install prefix (bin/ and lib/)")
    parser.add_argument("--lo", type=int, default=2420000000, help="RX LO, Hz")
    parser.add_argument("--rate", type=int, default=4000000, help="complex samples/s")
    parser.add_argument("--bandwidth", type=int, default=2000000, help="RX RF bandwidth, Hz")
    parser.add_argument("--rx-gain", type=float, default=0, help="manual RX gain, dB")
    parser.add_argument("--rx-port", default="A_BALANCED")
    parser.add_argument("--samples", type=int, default=4000000)


def from_args(args) -> SDR:
    return SDR(Settings(args.uri, args.lo, args.rate, args.bandwidth,
                        args.rx_gain, args.rx_port), args.libiio)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=pathlib.Path)
    add_arguments(parser)
    args = parser.parse_args()
    try:
        sdr = from_args(args)
        sdr.read_command(args.samples)
        sdr.configure()
        print(json.dumps(sdr.capture(args.capture, args.samples), allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(json.dumps({"error": str(exc)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
