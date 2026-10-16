#!/usr/bin/env python3
"""Decode an isolated CW ID window from int16 or float32 little-endian complex IQ.

iddump: morse.py id.iq --rate 10000 --tone 0 --expect N0CALL
RF capture: --tone 1000000 (plus measured carrier error), --start S --seconds S.
The narrow filter needs the tone frequency within roughly 100 Hz. A decode does
not establish RF frequency, power, spectral compliance or licence assignment.
"""
import argparse
import json
import pathlib
import sys

import numpy as np
from scipy.signal import butter, sosfilt

CODES = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/", (
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
    "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
    "..-", "...-", ".--", "-..-", "-.--", "--..",
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
    "-..-.")))
LETTERS = {v: k for k, v in CODES.items()}


def callsign(text: str) -> str:
    if not text.isascii():
        raise ValueError("callsign must be ASCII")
    call = text.upper()
    if (not 3 <= len(call) <= 23 or not call.isascii() or
            any(c not in CODES for c in call) or call.startswith("/") or
            call.endswith("/") or "//" in call or
            not any(c.isalpha() for c in call) or not any(c.isdigit() for c in call)):
        raise ValueError("callsign needs 3..23 ASCII letters/digits, optional single slashes, a letter and digit")
    return call


def duration(call: str) -> float:
    """One firmware ID including seven-dot leading/trailing guards, seconds."""
    units = 11
    for char in callsign(call):
        pattern = CODES[char]
        units += sum(3 if m == "-" else 1 for m in pattern) + len(pattern) + 2
    return units * .06


def decode(z: np.ndarray, rate: float, tone: float = 1e6, wpm: float = 20) -> dict:
    z = np.asarray(z)
    if (z.ndim != 1 or not np.isfinite(rate) or rate < 2000 or not np.isfinite(tone)
            or abs(tone) >= rate / 2 or not 1 <= wpm <= 20 or
            len(z) < rate * .5 or not np.all(np.isfinite(z))):
        raise ValueError("need finite IQ, Fs >= 2000, a tone inside Nyquist, 1..20 WPM and >=0.5 s")
    baseband = z * np.exp(-2j * np.pi * tone / rate * np.arange(len(z)))
    filtered = sosfilt(butter(4, 150, fs=rate, output="sos"), baseband)
    step = max(1, round(rate / 1000))
    env = abs(filtered[::step])
    peak = float(np.quantile(env, .95))
    floor = float(np.quantile(env, .1))
    if peak <= 0 or peak < 4 * floor:
        raise ValueError("no keyed tone above the envelope floor")
    on = env > floor + .45 * (peak - floor)
    boundaries = np.r_[0, np.flatnonzero(on[1:] != on[:-1]) + 1, len(on)]
    dot = 1.2 / wpm
    parts, pattern, marks, durations = [], "", 0, []
    for first, end in zip(boundaries[:-1], boundaries[1:]):
        units = (end - first) * step / rate / dot
        if on[first]:
            if first == 0 or end == len(on):
                raise ValueError("truncated Morse mark")
            if not (.65 <= units <= 1.35 or 2.65 <= units <= 3.35):
                raise ValueError(f"invalid Morse mark: {units:.2f} dots")
            pattern += "." if units < 2 else "-"
            marks += 1
            durations.append(units * dot)
        elif pattern:
            if units < 2:
                if not .65 <= units <= 1.35:
                    raise ValueError("invalid intra-character gap")
                continue
            if pattern not in LETTERS:
                raise ValueError(f"unknown Morse character {pattern}")
            parts.append(LETTERS[pattern])
            pattern = ""
            if units >= 5:
                parts.append(" ")
            elif not 2.65 <= units <= 3.35:
                raise ValueError("invalid character gap")
    if pattern or marks < 2:
        raise ValueError("incomplete Morse ID")
    text = "".join(parts).strip()
    return {"text": text, "marks": marks, "wpm_assumed": wpm,
            "tone_hz": tone, "sample_rate": rate,
            "mark_seconds_min": min(durations), "mark_seconds_max": max(durations),
            "method": "150 Hz low-pass, envelope threshold, fixed-speed Morse timing"}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture", type=pathlib.Path)
    p.add_argument("--rate", type=float, required=True)
    p.add_argument("--tone", type=float, default=1e6)
    p.add_argument("--format", choices=("i16", "f32"), default="i16")
    p.add_argument("--wpm", type=float, default=20)
    p.add_argument("--start", type=float, default=0)
    p.add_argument("--seconds", type=float)
    p.add_argument("--expect")
    args = p.parse_args()
    try:
        if args.start < 0 or (args.seconds is not None and args.seconds <= 0):
            raise ValueError("invalid capture window")
        dtype = np.dtype("<i2" if args.format == "i16" else "<f4")
        if args.capture.stat().st_size % (2 * dtype.itemsize):
            raise ValueError("partial I/Q sample")
        count = -1 if args.seconds is None else int(args.seconds * args.rate) * 2
        raw = np.fromfile(args.capture, dtype=dtype, count=count,
                          offset=int(args.start * args.rate) * 2 * dtype.itemsize).astype(float)
        report = decode(raw[::2] + 1j * raw[1::2], args.rate, args.tone, args.wpm)
        if args.expect:
            report["match"] = report["text"] == callsign(args.expect)
        print(json.dumps(report, allow_nan=False))
        return 0 if report.get("match", True) else 1
    except (ValueError, OSError) as exc:
        print(json.dumps({"error": str(exc)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
