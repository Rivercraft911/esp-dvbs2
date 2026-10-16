#!/usr/bin/env python3
"""Check the board's DVB-S2 samples with LeanDVB: a dvbdump over USB, or an SDR capture.

    python3 host/board.py "dvbdump 4 40" --save dump.iq --out runs.jsonl
    python3 host/onair.py dump.iq --rate 4000000 --scale 512 --stream 40
    python3 host/onair.py dump.iq --rate 40000000 --scale 512 --stream 2 --mer --shift 0
    python3 host/onair.py capture.iq --rate 4000000 --scale 2048 --stream 330 --auto-shift --mer
    python3 host/onair.py old-dump.iq --rate 4000000 --scale 512   # firmware 35739b5, 8-frame loop

With a configured station ID, pass --callsign CALL to verify the 40-byte ASCII
header at the start of every payload frame. Without it, legacy radio-off dumps
retain their existing checks. Crop RF captures to the data interval for this
tool; host/morse.py handles the surrounding CW intervals.

The GCS payload is one stream of xorshift32 bytes seeded with 2026: frame f carries bytes
[f * B, (f + 1) * B), B = (Kbch - 80) / 8. The firmware (firmware/main/dvbtx.c) encodes it
continuously; with --stream N (at least the number of frames the board sent) the decoded payload
must be a contiguous, bit-exact slice of the first N frames' bytes that starts on a frame
boundary. Filler frames, sent when the encoder is late, carry no payload and so keep the slice
contiguous. Without --stream the check is the one for firmware up to 35739b5, which looped
LOOP_FRAMES frames: one whole loop must occur in the decoded stream from its first byte.

Input is little-endian int16 I/Q; --scale is full scale (512 for the board's 10-bit words, 2048
for the E200's 12-bit ADC). --shift moves a carrier that sits that many Hz above the capture
center back to 0 Hz before decoding. --auto-shift estimates it from a fourth-power carrier line.
--mer recovers timing, clock drift, symbol position and a linear carrier phase, then compares
against the known symbols (the first N stream frames, or the 8-frame loop without --stream);
phase noise and channel distortion remain in the error, and filler frames are not modelled.
dump_mer() keeps the older aligned-dump measurement, which needs a whole number of samples per
symbol and is limited by its eighth-sample timing grid.
"""
from __future__ import annotations

import argparse
import functools
import json
import pathlib
import sys
import tempfile

import numpy as np
from scipy.ndimage import map_coordinates, spline_filter
from scipy.optimize import minimize_scalar
from scipy.signal import fftconvolve

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import decode  # noqa: E402
import dvbs2  # noqa: E402
import iq  # noqa: E402
import morse  # noqa: E402

LOOP_FRAMES = 8
SEED = 2026


def payload(modcod: int, frames: int = LOOP_FRAMES, callsign: str | None = None) -> bytes:
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
    if callsign:
        header = ("ESP-DVBS2 ID=" + morse.callsign(callsign)).ljust(39).encode("ascii") + b"\n"
        for first in range(0, len(out), per_frame):
            out[first:first + 40] = header
    return bytes(out)


@functools.lru_cache(maxsize=8)
def symbols(modcod: int, frames: int = LOOP_FRAMES, callsign: str | None = None) -> np.ndarray:
    """The first `frames` PL frames as complex symbols, from the independent Python encoder."""
    rate = dvbs2.RATES[modcod - 1]
    data = np.frombuffer(payload(modcod, frames, callsign), np.uint8)
    per_frame = len(data) // frames
    out = []
    for f in range(frames):
        bits = np.unpackbits(data[f * per_frame:(f + 1) * per_frame])
        fec = dvbs2.fecframe(dvbs2.bbframe(bits, rate), rate)
        out.append(dvbs2.plframe_phases(fec, rate))
    return np.exp(1j * np.pi / 4 * np.concatenate(out).astype(float))


def stream_match(stream: bytes, got: bytes, per_frame: int) -> int | None:
    """Offset of `got` in `stream` if it is whole frames starting on a frame boundary, else None."""
    if not got or len(got) % per_frame:
        return None
    at = stream.find(got)
    while at >= 0 and at % per_frame:
        at = stream.find(got, at + 1)
    return at if at >= 0 else None


def read_i16(path: pathlib.Path, scale: float) -> np.ndarray:
    if not np.isfinite(scale) or scale <= 0:
        raise ValueError("scale must be positive and finite")
    if pathlib.Path(path).stat().st_size % 4:
        raise ValueError("capture ends in a partial I/Q sample")
    raw = np.fromfile(path, dtype="<i2").astype(float) / scale
    return raw[0::2] + 1j * raw[1::2]


def carrier_offset(z: np.ndarray, rate: float, symbol_rate: float = 1e6) -> float:
    """QPSK fourth-power line, search +/-Rs/8 Hz; no assumption of a +55 kHz offset.

    Raising exp(j*2*pi*f*n/Fs) to power four puts its line at 4*f Hz. QPSK
    data has constant fourth power at symbol centers. A window limits leakage;
    the ratio to the median periodogram rejects noise-only acquisitions.
    """
    if not np.isfinite(rate) or not np.isfinite(symbol_rate) or rate < 2 * symbol_rate or symbol_rate <= 0:
        raise ValueError("need finite Fs >= 2*Rs > 0")
    z = np.asarray(z, dtype=complex)[:262144]
    if z.ndim != 1 or len(z) < 8192 or not np.all(np.isfinite(z)) or not np.any(z):
        raise ValueError("carrier acquisition needs >=8192 finite, nonzero samples")
    z = z - np.mean(z)
    z /= np.sqrt(np.mean(abs(z)**2))
    nfft = 1 << (len(z) - 1).bit_length()
    power = abs(np.fft.fft(z**4 * np.hanning(len(z)), nfft))**2
    freq = np.fft.fftfreq(nfft, 1 / rate)
    search = np.flatnonzero(abs(freq) < symbol_rate / 2)
    peak = int(search[np.argmax(power[search])])
    if power[peak] < 100 * np.median(power[search]):
        raise ValueError("no unambiguous QPSK fourth-power carrier line")
    a, b, c = np.log(np.maximum(power[(peak + np.array([-1, 0, 1])) % nfft], 1e-100))
    frac = .5 * (a - c) / (a - 2 * b + c)
    return float((freq[peak] + frac * rate / nfft) / 4)


def _sample(y: np.ndarray, times: np.ndarray, precise=False) -> np.ndarray:
    if not precise:
        return map_coordinates(y.real, [times], order=3, prefilter=False) + 1j * map_coordinates(
            y.imag, [times], order=3, prefilter=False)
    # Band-limited interpolation: 48-sample sinc, Kaiser beta 8.6. Normalize DC.
    out = np.empty(len(times), complex)
    for start in range(0, len(times), 16384):
        t = times[start:start + 16384]
        indices = np.floor(t).astype(int)[:, None] + np.arange(-23, 25)
        d = t[:, None] - indices
        w = np.sinc(d) * np.i0(8.6 * np.sqrt(np.maximum(0, 1 - (d / 24)**2))) / np.i0(8.6)
        out[start:start + len(t)] = np.sum(y[indices] * w, axis=1) / np.sum(w, axis=1)
    return out


def recover_mer(z: np.ndarray, rate: float, symbol_rate: float = 1e6,
                modcod: int = 4, shift: float | None = None, frames: int | None = None,
                callsign: str | None = None) -> dict:
    """Data-aided QPSK MER for an arbitrary interior of the transmitted symbols: the first
    `frames` stream frames, or the 8-frame loop when frames is None.

    Fit t[k] = t0 + k*s samples (one timing and clock fit over the capture), and
    phi[k] = phi0 + omega*k rad (one carrier fit). No equalizer or per-symbol
    phase correction is used. h = sum(conj(a)*r)/sum(|a|^2), and
    MER = 10*log10(sum(|h*a|^2)/sum(|r-h*a|^2)), a from symbols().
    Local 2048-symbol timing observations are gated at +/-500 ppm; all samples
    inside the filter/endpoint guards contribute to the final error, not just
    pilots or acquisition symbols. Reported clock error is relative to Fs/Rs.
    """
    z = np.asarray(z, dtype=complex)
    coarse = carrier_offset(z, rate, symbol_rate) if shift is None else float(shift)
    if (z.ndim != 1 or not np.isfinite(coarse) or not np.all(np.isfinite(z)) or not np.any(z)
            or not np.isfinite(rate) or not np.isfinite(symbol_rate)
            or rate < 2 * symbol_rate or symbol_rate <= 0):
        raise ValueError("invalid capture, rates or carrier shift")
    sps = rate / symbol_rate
    if len(z) < 8192 * sps:
        raise ValueError("MER needs at least 8192 symbols")
    loop = symbols(modcod, callsign=callsign) if frames is None else symbols(modcod, frames, callsign)
    z = z * np.exp(-2j * np.pi * coarse / rate * np.arange(len(z)))
    taps = iq.rrc(np.arange(-int(16 * sps), int(16 * sps) + 1) / sps, .35)
    y = fftconvolve(z, taps / np.sqrt(np.sum(taps**2)), mode="same")
    # Cache spline coefficients once; repeated interpolation must not refilter.
    spline = spline_filter(y.real) + 1j * spline_filter(y.imag)
    start = float(np.ceil(40 * sps))
    k = np.arange(1024)

    def eye(delay):
        r = _sample(spline, start + delay + k * sps)
        p = abs(r)**2
        return np.mean(p**2) / np.mean(p)**2

    grid = np.arange(16) * sps / 16
    delay = grid[np.argmin([eye(d) for d in grid])]
    delay = minimize_scalar(eye, bounds=(delay - sps / 16, delay + sps / 16),
                            method="bounded").x
    start += delay
    # Circular correlation gives the symbol number within the reference symbols.
    r = _sample(spline, start + k * sps)
    corr = np.fft.ifft(np.fft.fft(loop) * np.conj(np.fft.fft(r, len(loop))))
    offset = int(np.argmax(abs(corr)))
    score = float(abs(corr[offset]) / np.sqrt(len(r) * np.vdot(r, r).real))
    if score < .65:
        raise ValueError(f"known-symbol acquisition failed (correlation {score:.3f})")
    count = int((len(y) - 40 * sps - start) / sps)
    points = []
    predicted = 0.0
    for first in range(0, count - 1023, 2048):
        k = np.arange(first, min(first + 2048, count))
        a = loop[(offset + k) % len(loop)]
        if len(points) >= 2:
            slope = (points[-1][1] - points[-2][1]) / (points[-1][0] - points[-2][0])
            predicted = points[-1][1] + slope * (np.mean(k) - points[-1][0])
        elif points:
            predicted = points[-1][1]

        def cost(d):
            r = _sample(spline, start + k * sps + d)
            h = np.vdot(a, r) / len(a)
            return float(np.mean(abs(r - h * a)**2) / np.mean(abs(r)**2))

        fit = minimize_scalar(cost, bounds=(predicted - .4 * sps, predicted + .4 * sps),
                              method="bounded", options={"xatol": 1e-5})
        if fit.fun > .5:
            raise ValueError("lost known-symbol timing/carrier lock")
        points.append((float(np.mean(k)), float(fit.x)))
    centers, delays = np.array(points).T
    drift, timing = np.polyfit(centers, delays, 1)
    residual = float(np.max(abs(delays - (timing + drift * centers))))
    if abs(drift / sps) > 500e-6 or residual > .1 * sps:
        raise ValueError("clock fit outside +/-500 ppm or inconsistent timing observations")
    k = np.arange(count)
    times = start + timing + k * (sps + drift)
    valid = (times >= 40 * sps) & (times < len(y) - 40 * sps)
    k, times = k[valid], times[valid]
    r = _sample(y, times, precise=True)
    a = loop[(offset + k) % len(loop)]
    # Average phase before unwrapping, so a noisy individual symbol cannot slip.
    blocks = [slice(first, min(first + 1024, len(k))) for first in range(0, len(k), 1024)]
    phase = np.unwrap([np.angle(np.vdot(a[b], r[b])) for b in blocks])
    omega, phi = np.polyfit([np.mean(k[b]) for b in blocks], phase, 1)
    r *= np.exp(-1j * (phi + omega * k))
    h = np.vdot(a, r) / len(a)
    error = float(np.mean(abs(r - h * a)**2))
    signal = float(abs(h)**2)
    if signal <= error or not np.isfinite(error):
        raise ValueError("incoherent capture after carrier/timing fit")
    return {"mer_db": float(10 * np.log10(signal / max(error, 1e-100))),
            "carrier_offset_hz": float(coarse + omega * rate / (sps + drift) / (2 * np.pi)),
            "coarse_carrier_offset_hz": coarse, "symbol_offset": offset,
            "first_symbol_sample": float(start + timing), "samples_per_symbol": float(sps + drift),
            "sample_clock_offset_ppm": float(drift / sps * 1e6),
            "timing_fit_max_error_samples": residual, "acquisition_correlation": score,
            "symbols": len(k), "phase_noise_tracked": False,
            "method": "known symbols; RRC 0.35; affine timing; linear carrier; complex gain",
            "scope": "capture interior, no channel equalization; Fs/Rs is the clock reference"}


def capture_mer(path: pathlib.Path, rate: float, symbol_rate: float = 1e6,
                scale: float = 2048, modcod: int = 4, shift: float | None = None,
                frames: int | None = None, callsign: str | None = None) -> dict:
    return recover_mer(read_i16(path, scale), rate, symbol_rate, modcod, shift, frames, callsign)


def dump_mer(path: pathlib.Path, rate: float, symbol_rate: float, scale: float, modcod: int,
             stream: bool = False, callsign: str | None = None) -> dict:
    sps = rate / symbol_rate
    if sps != int(sps):
        raise ValueError("--mer needs a whole number of samples per symbol")
    sps = int(sps)
    raw = np.fromfile(path, dtype="<i2").astype(float) / scale
    z = raw[0::2] + 1j * raw[1::2]
    count = len(z) // sps
    if stream:
        frame = dvbs2.plframe_length(False, True)
        ideal = symbols(modcod, -(-count // frame), callsign)[:count]
    else:
        loop = symbols(modcod, callsign=callsign)
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
                best = (err, delay + frac, g)
    err, delay, g = best
    return {"mer_db": float(-10 * np.log10(err)), "delay_samples": float(delay), "symbols": count - 200,
            "rms_per_branch": float(np.sqrt(np.mean(raw ** 2)) * scale)}


def to_cf32(src: pathlib.Path, dst: pathlib.Path, rate: float, scale: float, shift: float) -> None:
    z = read_i16(src, scale)
    if shift:
        z = z * np.exp(-2j * np.pi * shift / rate * np.arange(z.size))
    z.astype(np.complex64).tofile(dst)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=pathlib.Path, help="int16 I/Q, little-endian")
    parser.add_argument("--rate", type=float, required=True, help="sample rate, samples/s")
    parser.add_argument("--scale", type=float, required=True, help="int16 full scale")
    shift_group = parser.add_mutually_exclusive_group()
    shift_group.add_argument("--shift", type=float, help="carrier offset to remove, Hz")
    shift_group.add_argument("--auto-shift", action="store_true", help="estimate the QPSK carrier offset")
    parser.add_argument("--callsign", help="configured callsign: verify the 40-byte header in every frame")
    parser.add_argument("--modcod", type=int, default=4)
    parser.add_argument("--symbol-rate", type=float, default=1e6)
    parser.add_argument("--receiver", type=pathlib.Path, help="local patched leandvb executable")
    parser.add_argument("--log", type=pathlib.Path, help="save unabridged receiver diagnostics")
    parser.add_argument("--stream", type=int, metavar="N",
                        help="continuous stream: check against the first N frames' payload")
    parser.add_argument("--mer", action="store_true", help="also recover timing/carrier and measure MER")
    parser.add_argument("--mer-only", action="store_true", help="measure MER without running LeanDVB")
    args = parser.parse_args()
    if args.stream is not None and args.stream < 1:
        parser.error("--stream needs at least one frame")
    try:
        mer = None
        if args.mer or args.mer_only:
            mer = capture_mer(args.capture, args.rate, args.symbol_rate, args.scale, args.modcod,
                              args.shift, args.stream, args.callsign)
            shift = mer["carrier_offset_hz"]
        else:
            shift = (carrier_offset(read_i16(args.capture, args.scale), args.rate, args.symbol_rate)
                     if args.auto_shift else (args.shift or 0.0))
        report = {}
        expected = payload(args.modcod, args.stream or LOOP_FRAMES, args.callsign)
        if not args.mer_only:
            with tempfile.TemporaryDirectory() as tmp:
                cf32 = pathlib.Path(tmp) / "capture.cfile"
                to_cf32(args.capture, cf32, args.rate, args.scale, shift)
                report, got = decode.decode(cf32, sample_rate=args.rate, symbol_rate=args.symbol_rate,
                                            alpha=0.35, receiver=args.receiver,
                                            expected=None if args.stream else expected,
                                            output=pathlib.Path(tmp) / "payload.bin", log=args.log)
            if args.stream:
                per_frame = len(expected) // args.stream
                at = stream_match(expected, got, per_frame)
                report.update({"stream_frames": args.stream, "stream_bytes": len(expected),
                               "payload_match": at is not None, "match_offset_bytes": at,
                               "first_frame": None if at is None else at // per_frame,
                               "payload_frames": len(got) // per_frame,
                               "compared_bits": 0 if at is None else len(got) * 8,
                               "payload_bit_errors": None if at is None else 0})
        report.update({"capture": str(args.capture), "shift_hz": shift, "scale": args.scale,
                       "callsign": morse.callsign(args.callsign) if args.callsign else None})
        if not args.stream:
            report["loop_frames"] = LOOP_FRAMES
        report.pop("command", None)
        if mer is not None:
            report["mer"] = mer
        print(json.dumps(report, sort_keys=True, allow_nan=False))
        return 0 if args.mer_only or (report["frame_lock"] and report.get("payload_match")
                                     and report.get("status_complete")) else 1
    except (OSError, ValueError, RuntimeError) as exc:
        print(json.dumps({"error": str(exc)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
