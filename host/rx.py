#!/usr/bin/env python3
"""Run the receiver firmware's `rx` command and check every payload against the transmitter.

    python3 host/rx.py 7 --mhz 2420 --gain 32 --seconds 30 --port PORT --callsign KJ5SCH
    python3 host/rx.py 7 --replay capture.sc8 --port PORT --callsign KJ5SCH

--replay uploads a recorded 16 MS/s int8 capture and runs it through the same demodulator
and FEC with the radio off, timing every push.

The receiver streams one binary record per short frame (firmware/rx/main/receive.h). The
transmitter's synthetic payload is a continuous xorshift32 stream seeded with 2026, with the
callsign in the first 40 bytes of every frame (host/decode.py payload_sequence). Each received
payload must equal the model frame that its sequence number implies. Exits 1 unless every frame
from the first decoded one to the last is present and exact.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import struct
import sys
import time

import morse
from dvbs2 import RATES
from dvbs2_tables import BCH_PARAMS

RECORD = struct.Struct("<IIihhHH")
MAGIC, END = 0x46524453, -2


def frame_bytes(modcod: int) -> int:
    """Data field bytes of a short QPSK frame: (Kbch - 80) / 8."""
    return (BCH_PARAMS["short"][RATES[modcod - 1]][0] - 80) // 8


def model(size: int, frames: int, callsign: str) -> list[bytes]:
    """The transmitter's first `frames` payloads, as host/decode.py generates them."""
    header = ("ESP-DVBS2 ID=" + morse.callsign(callsign)).ljust(39).encode("ascii") + b"\n"
    state, out = 2026, []
    for _ in range(frames):
        data = bytearray(size)
        for i in range(size):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            data[i] = state & 255
        data[:40] = header
        out.append(bytes(data))
    return out


def read_exact(link, n: int, deadline: float) -> bytes:
    data = b""
    while len(data) < n:
        if time.monotonic() > deadline:
            raise TimeoutError(f"stream stalled after {len(data)} of {n} bytes")
        data += link.read(n - len(data))
    return data


def receive(port: str, command: str, timeout: float):
    """Records before the stream, the frames, and records after it."""
    import board
    link = board.open_port(port, False)
    try:
        link.reset_input_buffer()
        link.write(command.encode() + b"\n")
        deadline = time.monotonic() + timeout
        before, frames, after, streaming, ended, line = [], [], [], False, False, b""
        while time.monotonic() < deadline:
            if streaming:
                head = RECORD.unpack(read_exact(link, RECORD.size, deadline))
                if head[0] != MAGIC:
                    raise ValueError(f"bad record magic {head[0]:#x} after {len(frames)} frames")
                if head[4] == END:
                    streaming, ended = False, True
                    continue
                payload = read_exact(link, head[4], deadline) if head[4] > 0 else b""
                frames.append(dict(zip(("sequence", "cfo_hz", "mer_cdb", "length", "iterations",
                                        "failed_checks"), head[1:]), payload=payload))
                continue
            line += link.read(1)
            if not line.endswith(b"\n"):
                continue
            text, line = line.strip(), b""
            if not text.startswith(b"@"):
                continue
            record = json.loads(text[1:])
            if record.get("done"):
                return before, frames, after
            (after if ended else before).append(record)
            if record.get("stream") == "binary":
                streaming = True
        raise TimeoutError("no 'done' from the receiver")
    finally:
        link.close()


def upload(port: str, data: bytes) -> dict:
    """Send a capture to the receiver's PSRAM; returns its CRC record."""
    import board
    import zlib
    link = board.open_port(port, False)
    try:
        link.reset_input_buffer()
        link.write(f"upload {len(data)}\n".encode())
        records, line, deadline = [], b"", time.monotonic() + 30 + len(data) / 50e3
        while time.monotonic() < deadline:
            line += link.read(1)
            if not line.endswith(b"\n"):
                continue
            text, line = line.strip(), b""
            if not text.startswith(b"@"):
                continue
            record = json.loads(text[1:])
            if record.get("error"):
                raise RuntimeError(record["error"])
            if record.get("ready"):
                link.write(data)
            if "crc32" in record:
                if record["crc32"] != zlib.crc32(data):
                    raise RuntimeError("upload CRC differs")
                return record
        raise TimeoutError("upload did not finish")
    finally:
        link.close()


def check(frames, modcod: int, callsign: str, tx_frames: int) -> dict:
    size = frame_bytes(modcod)
    expected = model(size, tx_frames, callsign)
    index = {payload: k for k, payload in enumerate(expected)}
    good = [f for f in frames if f["length"] == size]
    report = dict(frame_bytes=size, records=len(frames), decoded=len(good),
                  failed=sum(f["length"] < 0 for f in frames))
    if not good:
        return report | dict(exact=0, passed=False)
    k0 = index.get(good[0]["payload"])
    s0 = good[0]["sequence"]
    exact = sum(index.get(f["payload"]) == k0 + f["sequence"] - s0 for f in good) if k0 is not None else 0
    span = good[-1]["sequence"] - s0 + 1
    rate = 8 * size * 1e6 / 8370
    report.update(first_model_frame=k0, sequence_span=span, span_seconds=round(span * 8370 / 1e6, 3),
                  exact=exact, missing=span - len(good), payload_mbps=round(rate / 1e6, 4),
                  exact_bytes=exact * size, passed=k0 is not None and exact == span == len(good))
    return report


def main() -> int:
    import board
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("modcod", type=int, help="short-frame QPSK MODCOD, 4..10")
    p.add_argument("--mhz", type=int, default=2420)
    p.add_argument("--gain", type=int, default=32)
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--replay", type=pathlib.Path, help="recorded .sc8 capture to run with the radio off")
    p.add_argument("--port", default=board.DEFAULT_PORT)
    p.add_argument("--callsign", default="N0CALL")
    p.add_argument("--tx-frames", type=int, default=8000, help="model frames to search")
    p.add_argument("--out", type=pathlib.Path, help="append the summary to this JSON-lines file")
    a = p.parse_args()
    port = board.resolve_port(a.port)
    if a.replay:
        data = a.replay.read_bytes()
        command, timeout = f"replay {a.modcod}", 60 + len(data) / 1e6
    else:
        command, timeout = f"rx {a.mhz} {a.gain} {a.seconds} {a.modcod}", a.seconds + 60
    with board.reserve_port(port):
        if a.replay:
            upload(port, data)
        before, frames, after = receive(port, command, timeout)
    summary = dict(command=command, replay=str(a.replay) if a.replay else None, board=before + after,
                   check=check(frames, a.modcod, a.callsign, a.tx_frames),
                   frames=[{k: v for k, v in f.items() if k != "payload"} for f in frames[:3]])
    stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    print(json.dumps(summary["check"]))
    for record in after:
        print(json.dumps(record))
    if a.out:
        with a.out.open("a") as f:
            f.write(json.dumps({"host_time": stamp, **summary}) + "\n")
    return 0 if summary["check"]["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
