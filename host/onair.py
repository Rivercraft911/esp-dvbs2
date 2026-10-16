#!/usr/bin/env python3
"""Check the board's DVB-S2 samples with LeanDVB: a dvbdump over USB, or an SDR capture.

    python3 host/board.py "dvbdump 4 20" --save dump.iq --out runs.jsonl
    python3 host/onair.py dump.iq --rate 4000000 --scale 512
    python3 host/onair.py capture.iq --rate 4000000 --scale 2048 --shift 55000
    python3 host/onair.py dump.iq --rate 4000000 --scale 512 --mer      # dump only

The firmware (firmware/main/dvbtx.c) loops LOOP_FRAMES frames whose GCS payload is xorshift32
bytes seeded with 2026, continued across frames. The decoded stream must contain one whole loop,
starting at its first byte, bit for bit. Input is little-endian int16 I/Q; --scale is full scale
(512 for the board's 10-bit words, 2048 for the E200's 12-bit ADC). --shift moves a carrier that
sits that many Hz above the capture center back to 0 Hz before decoding. --mer compares a dump,
which starts on the first sample of symbol 0, with the symbols host/dvbs2.py encodes from the
same payload: RRC matched filter (roll-off 0.35, +-16 symbols), best delay in eighths of a
sample and complex gain.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import decode  # noqa: E402
import dvbs2  # noqa: E402
import iq  # noqa: E402

LOOP_FRAMES = 8
SEED = 2026


def payload(modcod: int, frames: int = LOOP_FRAMES) -> bytes:
    """GCS payload of `frames` QPSK frames, as dvbtx.c fills them."""
    if not 4 <= modcod <= 11:
        raise ValueError("QPSK modcods 4..11 only")
    per_frame = (dvbs2.code(dvbs2.RATES[modcod - 1]).kbch - 80) // 8
    out, x = bytearray(frames * per_frame), SEED
    for i in range(len(out)):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = x & 0xFF
    return bytes(out)


def symbols(modcod: int) -> np.ndarray:
    """One loop of PL frames as complex symbols, from the independent Python encoder."""
    rate = dvbs2.RATES[modcod - 1]
    data = np.frombuffer(payload(modcod), np.uint8)
    per_frame = len(data) // LOOP_FRAMES
    out = []
    for f in range(LOOP_FRAMES):
        bits = np.unpackbits(data[f * per_frame:(f + 1) * per_frame])
        fec = dvbs2.fecframe(dvbs2.bbframe(bits, rate), rate)
        out.append(dvbs2.plframe_phases(fec, rate))
    return np.exp(1j * np.pi / 4 * np.concatenate(out).astype(float))


def dump_mer(path: pathlib.Path, rate: float, symbol_rate: float, scale: float, modcod: int) -> dict:
    sps = rate / symbol_rate
    if sps != int(sps):
        raise ValueError("--mer needs a whole number of samples per symbol")
    sps = int(sps)
    raw = np.fromfile(path, dtype="<i2").astype(float) / scale
    z = raw[0::2] + 1j * raw[1::2]
    loop = symbols(modcod)
    count = len(z) // sps
    ideal = np.tile(loop, count // len(loop) + 1)[:count]
    best = None
    for frac in np.arange(8) / 8:               # the table pulse is centred between samples
        taps = iq.rrc((np.arange(-16 * sps, 16 * sps + 1) + frac) / sps, 0.35)
        y = np.convolve(z, taps / np.sqrt(np.sum(taps ** 2)), mode="same")
        for delay in range(0, 12 * sps):
            r = y[delay::sps][:count]
            a = ideal[:len(r)][100:-100]
            r = r[100:-100]
            g = np.vdot(r, a) / np.vdot(r, r)
            err = np.mean(np.abs(g * r - a) ** 2)
            if best is None or err < best[0]:
                best = (err, delay - frac, g)
    err, delay, g = best
    return {"mer_db": float(-10 * np.log10(err)), "delay_samples": float(delay), "symbols": count - 200,
            "rms_per_branch": float(np.sqrt(np.mean(raw ** 2)) * scale)}


def to_cf32(src: pathlib.Path, dst: pathlib.Path, rate: float, scale: float, shift: float) -> None:
    raw = np.fromfile(src, dtype="<i2")
    if raw.size % 2:
        raise ValueError("capture ends in a partial I/Q sample")
    z = (raw[0::2] + 1j * raw[1::2]) / scale
    if shift:
        z = z * np.exp(-2j * np.pi * shift / rate * np.arange(z.size))
    z.astype(np.complex64).tofile(dst)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=pathlib.Path, help="int16 I/Q, little-endian")
    parser.add_argument("--rate", type=float, required=True, help="sample rate, samples/s")
    parser.add_argument("--scale", type=float, required=True, help="int16 full scale")
    parser.add_argument("--shift", type=float, default=0.0, help="carrier offset to remove, Hz")
    parser.add_argument("--modcod", type=int, default=4)
    parser.add_argument("--symbol-rate", type=float, default=1e6)
    parser.add_argument("--receiver", type=pathlib.Path, help="local patched leandvb executable")
    parser.add_argument("--log", type=pathlib.Path, help="save unabridged receiver diagnostics")
    parser.add_argument("--mer", action="store_true", help="also measure MER of a dump")
    args = parser.parse_args()
    expected = payload(args.modcod)
    with tempfile.TemporaryDirectory() as tmp:
        cf32 = pathlib.Path(tmp) / "capture.cfile"
        to_cf32(args.capture, cf32, args.rate, args.scale, args.shift)
        report, _ = decode.decode(cf32, sample_rate=args.rate, symbol_rate=args.symbol_rate,
                                  alpha=0.35, receiver=args.receiver, expected=expected,
                                  output=pathlib.Path(tmp) / "payload.bin", log=args.log)
    report.update({"capture": str(args.capture), "shift_hz": args.shift, "scale": args.scale,
                   "loop_frames": LOOP_FRAMES})
    report.pop("command", None)
    if args.mer:
        report["mer"] = dump_mer(args.capture, args.rate, args.symbol_rate, args.scale, args.modcod)
    print(json.dumps(report, sort_keys=True))
    return 0 if report["frame_lock"] and report.get("payload_match") else 1


if __name__ == "__main__":
    sys.exit(main())
