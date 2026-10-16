#!/usr/bin/env python3
"""Send a command to the esp-dvbs2 firmware and collect its '@' JSON results.

    python3 host/board.py info
    python3 host/board.py membench --out results/2026-10-16-memory/runs.jsonl

Each record is printed, and with --out appended to a JSON-lines file together with the command
and the host time. The firmware marks the end of every command with {"done": true}.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import shutil
import subprocess
import sys
import time

import serial

DEFAULT_PORT = "/dev/cu.usbmodem101"


def open_port(port: str, reset: bool) -> serial.Serial:
    """Open the S3's USB serial port without resetting the chip, as idf.py monitor does.

    On this port RTS drives the chip's reset and DTR its GPIO0 boot strap. macOS asserts both when
    the port opens, so assert them first, then release RTS before DTR so that the change is never
    seen as a reset. Each RTS change re-sends DTR so the new line state reaches the device.

    An RTS reset from macOS left GPIO0 low and the chip in its bootloader (strap register 0x23,
    2026-10-16), so reset=True restarts the firmware with esptool's watchdog reset instead, which
    does not use DTR or RTS.
    """
    if reset:
        watchdog_reset(port)
    link = serial.Serial()
    link.port, link.baudrate, link.timeout = port, 115200, 0.1
    link.rts = link.dtr = True
    link.open()
    link.rts = False
    link.dtr = link.dtr
    link.dtr = False
    return link


def watchdog_reset(port: str) -> None:
    """Restart the firmware through esptool's RTC watchdog reset, then wait for it to boot."""
    python = shutil.which("esptool") and [shutil.which("esptool")]
    if not python:
        venvs = sorted(pathlib.Path.home().glob(".espressif/tools/python/*/venv/bin/python"))
        if not venvs:
            raise RuntimeError("esptool not found: activate ESP-IDF or install esptool")
        python = [str(venvs[-1]), "-m", "esptool"]
    subprocess.run(python + ["--chip", "esp32s3", "-p", port, "--after", "watchdog-reset",
                             "chip-id"], check=True, capture_output=True)
    time.sleep(2.5)                        # boot and USB driver start


def run(port: str, command: str, timeout: float = 30.0, reset: bool = False) -> list[dict]:
    link = open_port(port, reset)
    try:
        link.reset_input_buffer()
        link.write(command.encode() + b"\n")
        records, pending, deadline = [], b"", time.monotonic() + timeout
        while time.monotonic() < deadline:
            pending += link.read(4096)
            *lines, pending = pending.split(b"\n")
            for line in lines:
                line = line.strip()
                if not line.startswith(b"@"):
                    continue
                record = json.loads(line[1:])
                if record.get("done"):
                    return records
                records.append(record)
        raise TimeoutError(f"no 'done' from the board within {timeout} s for {command!r}")
    finally:
        link.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", help="firmware command line, e.g. info or membench")
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--reset", action="store_true", help="restart the firmware first")
    parser.add_argument("--out", type=pathlib.Path, help="append records to this JSON-lines file")
    args = parser.parse_args()

    records = run(args.port, args.command, args.timeout, args.reset)
    stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    for record in records:
        print(json.dumps(record))
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("a") as f:
            for record in records:
                f.write(json.dumps({"host_time": stamp, "command": args.command, **record}) + "\n")
    return 1 if any("error" in r for r in records) else 0


if __name__ == "__main__":
    sys.exit(main())
