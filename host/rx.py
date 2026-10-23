#!/usr/bin/env python3
"""Run the receiver firmware's `rx` command and check every payload against the transmitter.

    python3 host/rx.py 7 --mhz 2420 --gain 32 --seconds 30 --port PORT --callsign KJ5SCH
    python3 host/rx.py 16 --rate 5m --seconds 30 --port PORT --callsign KJ5SCH
    python3 host/rx.py 7 --replay capture.sc8 --port PORT --callsign KJ5SCH

--replay uploads a recorded 16 MS/s int8 capture and runs it through the same receiver with
the radio off, paced as the radio would deliver it, timing every push.

The receiver streams one binary record per short frame (firmware/rx/main/receive.h): the
payload at 1 Msym/s, its CRC-32 at 5 Msym/s. The transmitter's synthetic payload is a
continuous xorshift32 stream seeded with 2026, with the callsign in the first 40 bytes of every
frame (host/decode.py payload_sequence). Each received payload (or CRC) must equal the model
frame's that its sequence number implies. Exits 1 unless every frame from the first decoded one
to the last is present and exact.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import struct
import sys
import time
import zlib

import morse
from dvbs2 import PSK8_RATES, RATES
from dvbs2_tables import BCH_PARAMS

RECORD = struct.Struct("<IIihhHH")
LLR_BYTES, BIT_BYTES = 16200, 2025
MAGIC, END = 0x46524453, -2
SYMBOL_RATE = {"1m": 1e6, "5m": 5e6}


def frame_bytes(modcod: int) -> int:
    """Data field bytes of a short frame: (Kbch - 80) / 8. QPSK MODCODs 4..10, 8PSK 12..16."""
    rate = PSK8_RATES[modcod - 12] if modcod >= 12 else RATES[modcod - 1]
    return (BCH_PARAMS["short"][rate][0] - 80) // 8


def frame_symbols(modcod: int) -> int:
    """A short PLFRAME with pilots: 8,370 symbols for QPSK, 5,598 for 8PSK."""
    return 5598 if modcod >= 12 else 8370


def stream(n: int) -> bytes:
    """The first n bytes of the transmitter's xorshift32 stream (seed 2026), cached in the
    temp directory because 5 Msym/s runs need about 100 MB of it."""
    import tempfile
    path = pathlib.Path(tempfile.gettempdir()) / "esp-dvbs2-xorshift.bin"
    if path.exists() and path.stat().st_size >= n:
        with path.open("rb") as f:
            return f.read(n)
    out, state = bytearray(n), 2026
    for i in range(n):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        out[i] = state & 255
    path.write_bytes(out)
    return bytes(out)


def model(size: int, frames: int, callsign: str) -> list[bytes]:
    """The transmitter's first `frames` payloads, as host/decode.py generates them: one
    continuous stream cut into frames, each starting with the station header."""
    header = ("ESP-DVBS2 ID=" + morse.callsign(callsign)).ljust(39).encode("ascii") + b"\n"
    data = stream(size * frames)
    return [header + data[k * size + 40:(k + 1) * size] for k in range(frames)]


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
        crc, bits = False, False
        while time.monotonic() < deadline:
            if streaming:
                head = RECORD.unpack(read_exact(link, RECORD.size, deadline))
                if head[0] != MAGIC:
                    raise ValueError(f"bad record magic {head[0]:#x} after {len(frames)} frames")
                if head[4] == END:
                    streaming, ended = False, True
                    continue
                frame = dict(zip(("sequence", "cfo_hz", "mer_cdb", "length", "iterations",
                                  "failed_checks"), head[1:]), payload=b"")
                if head[4] > 0 and crc:
                    frame["crc"] = struct.unpack("<I", read_exact(link, 4, deadline))[0]
                elif head[4] > 0:
                    frame["payload"] = read_exact(link, head[4], deadline)
                if head[4] == -1:
                    frame["header_sample"] = struct.unpack("<Q", read_exact(link, 8, deadline))[0]
                    if bits:
                        frame["hard"] = read_exact(link, BIT_BYTES, deadline).hex()
                        frame["weak"] = read_exact(link, BIT_BYTES, deadline).hex()
                    else:
                        frame["llr"] = read_exact(link, LLR_BYTES, deadline).hex()
                frames.append(frame)
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
                crc = record.get("payload") == "crc32"
                bits = record.get("failed") == "hard+weak"
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


def check(frames, modcod: int, callsign: str, tx_frames: int, symbol_rate: float = 1e6) -> dict:
    """Frames carry a payload, or at 5 Msym/s its CRC-32 ("crc"); either must match the model
    frame that the sequence implies. Records arrive in sequence order except decoded ones."""
    size = frame_bytes(modcod)
    expected = model(size, tx_frames, callsign)
    key = (lambda f: f["crc"]) if any("crc" in f for f in frames) else (lambda f: f["payload"])
    index = ({zlib.crc32(p): k for k, p in enumerate(expected)} if any("crc" in f for f in frames)
             else {p: k for k, p in enumerate(expected)})
    good = sorted((f for f in frames if f["length"] == size), key=lambda f: f["sequence"])
    report = dict(frame_bytes=size, records=len(frames), decoded=len(good),
                  failed=sum(f["length"] < 0 for f in frames))
    if not good:
        return report | dict(exact=0, passed=False)
    k0 = index.get(key(good[0]))
    s0 = good[0]["sequence"]
    exact = sum(index.get(key(f)) == k0 + f["sequence"] - s0 for f in good) if k0 is not None else 0
    span = good[-1]["sequence"] - s0 + 1
    period = frame_symbols(modcod) / symbol_rate
    found = [k for k in (index.get(key(f)) for f in good) if k is not None]
    report.update(valid=len(found), in_order=all(b > a for a, b in zip(found, found[1:])),
                  model_span=found[-1] - found[0] + 1 if found else 0,
                  lost=found[-1] - found[0] + 1 - len(found) if found else 0)
    report.update(first_model_frame=k0, sequence_span=span, span_seconds=round(span * period, 3),
                  exact=exact, missing=span - len(good), payload_mbps=round(8 * size / period / 1e6, 4),
                  exact_bytes=exact * size, passed=k0 is not None and exact == span == len(good))
    return report


def main() -> int:
    import board
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("modcod", type=int, help="short-frame MODCOD: QPSK 4..10, 8PSK 12..16")
    p.add_argument("--rate", choices=SYMBOL_RATE, default="1m", help="symbols per second")
    p.add_argument("--mhz", type=int, default=2420)
    p.add_argument("--gain", type=int, default=32)
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--replay", type=pathlib.Path, help="recorded .sc8 capture to run with the radio off")
    p.add_argument("--skip", type=int, default=0, help="replay: leave out every SKIP-th push")
    p.add_argument("--port", default=board.DEFAULT_PORT)
    p.add_argument("--callsign", default="N0CALL")
    p.add_argument("--tx-frames", type=int, help="model frames to search (default: 60 s of the rate)")
    p.add_argument("--out", type=pathlib.Path, help="append the summary to this JSON-lines file")
    a = p.parse_args()
    port = board.resolve_port(a.port)
    token = " 5m" if a.rate == "5m" else ""
    if a.replay:
        data = a.replay.read_bytes()
        command = f"replay {a.modcod}" + (f" {a.skip}" if a.skip else "") + token
        timeout = 60 + len(data) / 1e6
    else:
        command, timeout = f"rx {a.mhz} {a.gain} {a.seconds} {a.modcod}{token}", a.seconds + 60
    with board.reserve_port(port):
        if a.replay:
            upload(port, data)
        before, frames, after = receive(port, command, timeout)
    summary = dict(command=command, replay=str(a.replay) if a.replay else None, board=before + after,
                   check=check(frames, a.modcod, a.callsign,
                               a.tx_frames or int(60 * SYMBOL_RATE[a.rate] / frame_symbols(a.modcod)) + 100,
                               SYMBOL_RATE[a.rate]),
                   frames=[{k: v for k, v in f.items() if k != "payload"} for f in frames[:3]],
                   failed=[{k: v for k, v in f.items() if k != "payload"} | {"index": n}
                           for n, f in enumerate(frames) if f["length"] < 0],
                   around_failed=[{k: v for k, v in frames[m].items() if k not in ("payload", "llr", "hard", "weak")}
                                  for n, f in enumerate(frames) if f["length"] < 0
                                  for m in range(max(0, n - 2), min(len(frames), n + 3))])
    stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    print(json.dumps(summary["check"]))
    for record in summary["failed"]:
        print(json.dumps({"failed_frame": {k: v for k, v in record.items() if k not in ("llr", "hard", "weak")}}))
    for record in after:
        print(json.dumps(record))
    if a.out:
        with a.out.open("a") as f:
            f.write(json.dumps({"host_time": stamp, **summary}) + "\n")
    return 0 if summary["check"]["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
