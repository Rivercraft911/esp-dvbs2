#!/usr/bin/env python3
"""Send a command to the esp-dvbs2 firmware and collect its '@' JSON results.

    python3 host/board.py info
    python3 host/board.py "dvbpace 11 26m667 10" --out notes/pace.jsonl
    python3 host/board.py "dvbdump 4 16" --save dump.iq
    python3 host/board.py "dvbpace 14 5m 30 --short" --monitor 127.0.0.1:1235

Each record is printed, and with --out appended to a JSON-lines file together with the command
and the host time. The firmware marks the end of every command with {"done": true}. A record
with "binary_bytes": N is followed by exactly N raw bytes, which --save writes to a file.
The `status` records that dvb and dvbpace print (once a second up to 10 Msym/s, a single one
above) are not results: they are neither printed nor kept, and --monitor forwards them, with
the command's records, to host/monitor.py.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import time
import tempfile
import zlib

import serial
from serial.tools import list_ports

DEFAULT_PORT = "/dev/cu.usbmodem101"


def resolve_port(port: str = DEFAULT_PORT) -> str:
    """Keep explicit ports; a missing default can use one unambiguous native USB port."""
    if port == DEFAULT_PORT and not pathlib.Path(port).exists():
        ports = sorted(pathlib.Path('/dev').glob('cu.usbmodem*'))
        if len(ports) == 1:
            return str(ports[0])
    return port


def reserve_port(port: str, reset: bool = False):
    """Lock the port and reject existing cu/tty users before any reset/open.

    A parent process can pass this locked descriptor to its board.py children.
    lsof also catches tools that do not participate in our advisory lock.
    """
    device = os.path.realpath(port).replace('/dev/tty.', '/dev/cu.')
    name = hashlib.sha256(device.encode()).hexdigest()[:16]
    path = pathlib.Path(tempfile.gettempdir()) / ('esp-dvbs2-' + name + '.lock')
    inherited = os.getenv('ESP_DVBS2_LOCK_FD')
    lock = os.fdopen(os.dup(int(inherited)), 'a+') if inherited else path.open('a+')
    try:
        assert os.fstat(lock.fileno()).st_ino == path.stat().st_ino, 'wrong board lock'
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        ports = [device]
        if reset:
            bridge = bridge_port()
            if bridge: ports.append(bridge)
        aliases = sorted({p for dev in ports for p in
                          (dev, dev.replace('/dev/cu.', '/dev/tty.')) if pathlib.Path(p).exists()})
        if aliases:
            result = subprocess.run(['lsof', '-t', '--', *aliases], capture_output=True, text=True)
            if result.returncode not in (0, 1) or result.stderr.strip():
                raise RuntimeError('cannot check serial ownership: ' + result.stderr.strip())
            if result.stdout.strip():
                raise RuntimeError('serial port busy; owner PIDs: ' + result.stdout.strip().replace('\n', ', '))
        return lock
    except BaseException:
        lock.close()
        raise


def open_port(port: str, reset: bool) -> serial.Serial:
    """Open the S3's USB serial port without resetting the chip, as idf.py monitor does.

    On this port RTS drives the chip's reset and DTR its GPIO0 boot strap. macOS asserts both when
    the port opens, so assert them first, then release RTS before DTR so that the change is never
    seen as a reset. Each RTS change re-sends DTR so the new line state reaches the device.

    An RTS reset through this port from macOS left GPIO0 low and the chip in its bootloader (strap
    register 0x23, 2026-10-16). So reset=True pulls EN low through the board's USB-UART bridge if
    it is plugged in, and otherwise uses esptool's watchdog reset.
    """
    if reset:
        bridge = bridge_port()
        en_reset(bridge) if bridge else watchdog_reset(port)
    link = serial.Serial()
    link.port, link.baudrate, link.timeout = port, 115200, 0.1
    link.rts = link.dtr = True
    link.open()
    link.rts = False
    link.dtr = link.dtr
    link.dtr = False
    return link


def bridge_port() -> str | None:
    """The board's second USB port: a WCH USB-UART bridge (VID 0x1A86) on UART0, if connected."""
    for p in list_ports.comports():
        if p.vid == 0x1A86:
            return p.device
    return None


def en_reset(bridge: str) -> None:
    """Hardware reset through the bridge's auto-reset circuit: RTS high with DTR low pulls EN
    low; equal levels leave EN and GPIO0 alone, so the chip restarts into the firmware."""
    with serial.Serial() as link:
        link.port, link.baudrate = bridge, 115200
        link.dtr = link.rts = False
        link.open()
        link.dtr, link.rts = False, True
        time.sleep(0.1)
        link.rts = False
    time.sleep(2.5)                        # boot and USB driver start


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


class Monitor:
    """Forward records to host/monitor.py as UDP datagrams of type 4, transmitter stats: a 12-byte
    little-endian header (b'PVMN', version 1, type, a sequence number and the host's monotonic
    clock in microseconds, each cut to its width), then the record as JSON. A send that fails is
    counted and skipped, so the monitor can never stop a run's records."""
    HEADER = struct.Struct('<4sBBHI')
    TYPE = 4

    def __init__(self, address: str):
        host, _, port = address.rpartition(':')
        host = host.strip('[]')
        if not host or not 0 < int(port) < 65536:
            raise ValueError(f'expected HOST:PORT, got {address!r}')
        self.target = (host, int(port))
        self.socket = socket.socket(socket.AF_INET6 if ':' in host else socket.AF_INET, socket.SOCK_DGRAM)
        self.seq = self.failed = 0

    def __call__(self, record: dict) -> None:
        stamp = time.monotonic_ns() // 1000
        head = self.HEADER.pack(b'PVMN', 1, self.TYPE, self.seq & 0xFFFF, stamp & 0xFFFFFFFF)
        self.seq += 1
        try:
            self.socket.sendto(head + json.dumps(record, separators=(',', ':')).encode(), self.target)
        except OSError:
            self.failed += 1


def run(port: str, command: str, timeout: float = 30.0, reset: bool = False,
        save: pathlib.Path | None = None, live=None) -> list[dict]:
    """Collect '@' records until done. Raw bytes announced by binary_bytes go to save, and the
    record gains host_crc32 of what arrived. The deadline moves while bytes keep arriving.
    live(record), if given, sees every record as it arrives; `status` records go only there."""
    link = open_port(port, reset)
    sink, left, received = None, 0, 0
    try:
        link.reset_input_buffer()
        link.write(command.encode() + b"\n")
        records, pending, device_text, deadline = [], b"", b"", time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = link.read(65536)
            if chunk:
                deadline = max(deadline, time.monotonic() + 10)
            pending += chunk
            while pending:
                if left:
                    part, pending = pending[:left], pending[left:]
                    sink.write(part)
                    received = zlib.crc32(part, received)
                    left -= len(part)
                    if not left:
                        sink.close()
                        records[-1]["host_crc32"] = received
                    continue
                end = pending.find(b"\n")
                if end < 0:
                    break
                line, pending = pending[:end].strip(), pending[end + 1:]
                if not line.startswith(b"@"):
                    device_text = (device_text + line + b"\n")[-4096:]
                    continue
                record = json.loads(line[1:])
                if record.get("done"):
                    return records
                if live:
                    live(record)
                if record.get("cmd") == "status":
                    continue
                records.append(record)
                if record.get("binary_bytes"):
                    if save is None:
                        raise ValueError("the board sends binary data: pass --save FILE")
                    sink, left, received = open(save, "wb"), record["binary_bytes"], 0
        raise TimeoutError(f"no 'done' from the board within {timeout} s for {command!r}; "
                           f"records={records!r}; device text="
                           f"{(device_text + pending)[-4096:].decode(errors='replace')!r}")
    finally:
        if sink and not sink.closed:
            sink.close()
        link.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", help="firmware command line, e.g. info or dvbdump 4 16")
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--reset", action="store_true", help="restart the firmware first")
    parser.add_argument("--out", type=pathlib.Path, help="append records to this JSON-lines file")
    parser.add_argument("--save", type=pathlib.Path, help="write binary data from the board here")
    parser.add_argument("--monitor", type=Monitor, metavar="HOST:PORT",
                        help="also send status and result records to host/monitor.py over UDP")
    args = parser.parse_args()
    args.port = resolve_port(args.port)

    with reserve_port(args.port, args.reset):
        records = run(args.port, args.command, args.timeout, args.reset, args.save, args.monitor)
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
