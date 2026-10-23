#!/usr/bin/env python3
"""Run the receiver firmware's `rx` command and check every payload against the transmitter.

    python3 host/rx.py 7 --mhz 2420 --gain 32 --seconds 30 --port PORT --callsign N0CALL
    python3 host/rx.py 16 --rate 5m --seconds 30 --port PORT --callsign N0CALL
    python3 host/rx.py 7 --replay capture.sc8 --port PORT --callsign N0CALL
    python3 host/rx.py 16 --rate 5m --ts replay --seconds 30 --port PORT

--replay uploads a recorded 16 MS/s int8 capture and runs it through the same receiver with
the radio off, paced as the radio would deliver it, timing every push.

The receiver streams one binary record per short frame (firmware/rx/main/receive.h): the
payload at 1 Msym/s, its CRC-32 at 5 Msym/s. The transmitter's synthetic payload is a
continuous xorshift32 stream seeded with 2026, with the callsign in the first 40 bytes of every
frame (host/decode.py payload_sequence). Each received payload (or CRC) must equal the model
frame's that its sequence number implies. Exits 1 unless every frame from the first decoded one
to the last is present and exact.

--ts: the transmitter sends its TS test input (`spiinput ... ts replay`, or a live sender's
counting pattern), the model is that input's TS BBFRAMEs (ts_model), and the receiver also
deframes every frame and checks each packet against the pattern (`rx ... ts pattern`). Then the
receiver's TS counters must account for every exact frame and the packets in them, and be clean:
no CRC-8 errors, pattern errors or jumps, restarts or late frames, and continuity errors only
where the replay loops (it repeats 56 packets, not a multiple of 16).
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


CRC8 = [0] * 256
for _v in range(256):
    _c = _v
    for _ in range(8):
        _c = ((_c << 1) ^ (0xD5 if _c & 0x80 else 0)) & 0xFF
    CRC8[_v] = _c


def ts_crc8(packet: bytes) -> int:
    """CRC-8 of a packet's 187 bytes after the sync byte (5.1.4), as host/dvbs2.py crc8."""
    c = 0
    for b in packet[1:188]:
        c = CRC8[c ^ b]
    return c


def ts_messages(first: int, count: int, replay: bool) -> list[bytes]:
    """The transmitter's TS test input, 7 packets per PV message (host/spi_input.py pattern).
    `spiinput ... ts replay` repeats the messages of sequence 0..7 (ingress_replay_fill); a live
    sender counts its sequence on from 0, wrapping at 65,536."""
    from spi_input import pattern
    cache = {}
    out = []
    for m in range(first, first + count):
        seq = m % 8 if replay else m % 65536
        if seq not in cache:
            cache[seq] = pattern(seq, 1)
        out.append(cache[seq])
    return out


def ts_model(modcod: int, frames: int, replay: bool = True, short: bool = True) -> list[bytes]:
    """Data fields of the transmitter's first `frames` TS frames for that input: the sync byte of
    each packet replaced by the CRC-8 of the packet before it (the first by 0), cut into fields of
    Kbch - 80 bits, as host/dvbs2.py ts_bbframes does (tests/test_rx_host.py checks the two
    agree)."""
    rate = PSK8_RATES[modcod - 12] if modcod >= 12 else RATES[modcod - 1]
    size = (BCH_PARAMS["short" if short else "normal"][rate][0] - 80) // 8
    messages = -(-size * frames // 1316) + 1
    if replay:
        cycle = b"".join(ts_messages(0, 8, True))
        crcs = [ts_crc8(cycle[188 * i:188 * i + 188]) for i in range(56)]
        period = bytearray(cycle)
        for i in range(56):
            period[188 * i] = crcs[i - 1]
        stream = bytearray(period * (-(-messages // 8)))
    else:
        stream = bytearray(b"".join(ts_messages(0, messages, False)))
        crcs = [ts_crc8(stream[i:i + 188]) for i in range(0, len(stream), 188)]
        for i in range(1, len(crcs)):
            stream[188 * i] = crcs[i - 1]
    stream[0] = 0
    return [bytes(stream[k * size:(k + 1) * size]) for k in range(frames)]


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


def check(frames, modcod: int, callsign: str, tx_frames: int, symbol_rate: float = 1e6,
          payloads: list[bytes] | None = None) -> dict:
    """Frames carry a payload, or at 5 Msym/s its CRC-32 ("crc"); either must match the model
    frame that the sequence implies. Records arrive in sequence order except decoded ones.
    Tens of thousands of model frames share some CRC-32s (birthday collisions), so a frame is
    compared with the model frame its sequence implies, and the lookup is only a fallback.
    payloads: the transmitter's data fields, if not its synthetic GCS stream (ts_model)."""
    size = frame_bytes(modcod)
    expected = payloads if payloads is not None else model(size, tx_frames, callsign)
    crc = any("crc" in f for f in frames)
    key = (lambda f: f["crc"]) if crc else (lambda f: f["payload"])
    keys = [zlib.crc32(p) for p in expected] if crc else expected
    index = {}
    for k, v in enumerate(keys):
        index.setdefault(v, k)
    good = sorted((f for f in frames if f["length"] == size), key=lambda f: f["sequence"])
    report = dict(frame_bytes=size, records=len(frames), decoded=len(good),
                  failed=sum(f["length"] < 0 for f in frames))
    if not good:
        return report | dict(exact=0, passed=False)
    k0 = index.get(key(good[0]))
    s0 = good[0]["sequence"]
    implied = lambda f: k0 + f["sequence"] - s0
    matches = [k0 is not None and 0 <= implied(f) < len(keys) and keys[implied(f)] == key(f) for f in good]
    found = [implied(f) if ok else index.get(key(f)) for f, ok in zip(good, matches)]
    found = [k for k in found if k is not None]
    exact = sum(matches)
    span = good[-1]["sequence"] - s0 + 1
    period = frame_symbols(modcod) / symbol_rate
    mers = sorted(f["mer_cdb"] for f in good if "mer_cdb" in f)     # pilot MER: lowest, median
    report.update(mislabelled=[dict(sequence=f["sequence"], model=index.get(key(f)), implied=implied(f))
                               for f, ok in zip(good, matches) if k0 is not None and not ok][:8],
                  valid=len(found), in_order=all(b > a for a, b in zip(found, found[1:])),
                  model_span=found[-1] - found[0] + 1 if found else 0,
                  lost=found[-1] - found[0] + 1 - len(found) if found else 0,
                  first_model_frame=k0, sequence_span=span, span_seconds=round(span * period, 3),
                  mer_db=[mers[0] / 100, mers[len(mers) // 2] / 100] if mers else None,
                  exact=exact, missing=span - len(good), payload_mbps=round(8 * size / period / 1e6, 4),
                  missing_sequences=sorted(set(range(s0, s0 + span)) - {f["sequence"] for f in good})[:8],
                  exact_bytes=exact * size, passed=k0 is not None and exact == span == len(good))
    return report


def ts_packets_expected(first_frame: int, frames: int, size: int) -> tuple[int, int]:
    """Packets a deframer must hand on from model frames first_frame .. first_frame + frames - 1
    received in a row, data fields of `size` bytes: those that start at or after the first packet
    start in the range and whose CRC-8 (the next packet's first byte) is in it, and the one ending
    exactly at the end of the range, unchecked at the flush. Returns (all, unchecked)."""
    lo, hi = first_frame * size, (first_frame + frames) * size
    first = -(-lo // 188)
    checked = max(0, (hi - 1) // 188 - first)
    unchecked = int(hi % 188 == 0 and hi // 188 - 1 >= first)
    return checked + unchecked, unchecked


def ts_verdict(records, replay: bool, frames: dict) -> dict:
    """The receiver's TS counters (`rx ... ts pattern` summary) against the frames check(): every
    good frame went through the deframer (frames the LDPC decoder repaired come too late and do
    not), and it handed on exactly the packets those frames hold. Then the counters are clean."""
    summary = next((r for r in records if isinstance(r.get("ts"), dict)), None)
    if summary is None:
        return dict(passed=False, error="no ts counters")
    t, p = summary["ts"], summary["ts"]["pattern"]
    k0, span, size = frames.get("first_model_frame"), frames.get("sequence_span", 0), frames["frame_bytes"]
    want, want_unchecked = ts_packets_expected(k0, span, size) if k0 is not None else (None, None)
    deframed = frames.get("decoded", 0) - summary.get("decoded", 0)
    clean = (t["crc_errors"] == p["bad"] == p["jumps"] == t["resyncs"] == t["late"] == t["not_ts"] == 0
             and t["cc_errors"] == (p["wraps"] if replay else 0) and t["unchecked"] == want_unchecked
             and p["checked"] == t["packets"] == want and t["frames"] == deframed > 0)
    return dict(packets=t["packets"], expected_packets=want, frames=t["frames"], expected_frames=deframed,
                crc_errors=t["crc_errors"], cc_errors=t["cc_errors"], replay_wraps=p["wraps"],
                pattern_bad=p["bad"], pattern_jumps=p["jumps"], resyncs=t["resyncs"],
                unchecked=t["unchecked"], null_packets=t["null_packets"], passed=clean)


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
    p.add_argument("--ts", choices=("replay", "live"),
                   help="TS test input: the transmitter's `ts replay` loop, or a live sender's count")
    p.add_argument("--tx-frames", type=int, help="model frames to search (default: 60 s of the rate)")
    p.add_argument("--out", type=pathlib.Path, help="append the summary to this JSON-lines file")
    a = p.parse_args()
    port = board.resolve_port(a.port)
    token = (" 5m" if a.rate == "5m" else "") + (" ts pattern" if a.ts else "")
    if a.replay:
        data = a.replay.read_bytes()
        command = f"replay {a.modcod}" + (f" {a.skip}" if a.skip else "") + token
        timeout = 60 + len(data) / 1e6
    else:
        command, timeout = f"rx {a.mhz} {a.gain} {a.seconds} {a.modcod}{token}", a.seconds + 60
    with board.reserve_port(port):
        firmware = next((r.get("firmware") for r in board.run(port, "info", 10) if "firmware" in r), None)
        if a.replay:
            upload(port, data)
        before, frames, after = receive(port, command, timeout)
    tx_frames = a.tx_frames or int(60 * SYMBOL_RATE[a.rate] / frame_symbols(a.modcod)) + 100
    payloads = ts_model(a.modcod, tx_frames, a.ts == "replay") if a.ts else None
    result = check(frames, a.modcod, a.callsign, tx_frames, SYMBOL_RATE[a.rate], payloads)
    if a.ts:
        result["ts"] = ts_verdict(after, a.ts == "replay", result)
        result["passed"] = result["passed"] and result["ts"]["passed"]
    summary = dict(firmware=firmware, command=command, replay=str(a.replay) if a.replay else None,
                   board=before + after, check=result,
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
