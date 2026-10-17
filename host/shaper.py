#!/usr/bin/env python3
"""Pulse shaping for the S3 DAC: pulse design, tables, sample model, mask and MER checks.

    python3 host/shaper.py report             margins, MER and table sizes of every profile
    python3 host/shaper.py header             regenerate firmware/main/shaper_taps.h
    python3 host/shaper.py verify RUNS.jsonl  compare membench shaper records with this model

How samples are made. The DAC plays one 32-bit word per complex sample at 40 MS/s: signed 10-bit
I in bits 9:0 and Q in bits 19:10 (esp-sdr-zephyr include/esp_sdr/esp_sdr_tx.h:84-88 at
3214c16; confirmed on the board by the -1 MHz test signal in results/2026-10-16-radio).
Output sample p of symbol n is

    y[nN + p] = sum_{a=0}^{L-1} x[n - a] g[aN + p],    x = exp(j pi k / 4), k = 0..7,

with N samples per symbol and a pulse g of L symbols. Every possible output is precomputed. One
table indexed by the whole 9-symbol history would need 8^9 rows, so the sum is split by age into
three parts of three symbols (L = 9). Part j (ages 3j..3j+2) depends on a 3-symbol window, which
is the window part 0 saw three symbols earlier. A window's table block therefore holds its
part 0, 1 and 2 rows back to back; the kernel reads part 0 of window n, part 1 of window n - 3
and part 2 of window n - 6 and adds them.

Each part row holds packed words (I + bias) | (Q + bias) << 10 with per-part biases that sum to
512 and keep every field between 0 and 1023, so a plain 32-bit add of the three rows never
carries from I into Q. The sum is offset binary; XOR 0x80200 turns it into two's complement.
Each part is rounded on its own, so a sample is within 1.5 LSB of the exact filter output.

Symbol input: one byte per symbol, 16 * k for the phase k * pi / 4 (DVB-S2 8PSK and QPSK points
are all on this grid; QPSK uses k = 1, 3, 5, 7). The caller keeps the 8 previous symbols in
front of the first one.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import dataclass
from functools import cached_property

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "firmware" / "main" / "shaper_taps.h"

I_SHIFT, Q_SHIFT = 0, 10       # DAC word fields (see module docstring)
XOR = (512 << I_SHIFT) | (512 << Q_SHIFT)       # 0x80200: offset binary -> two's complement
L, PARTS, SPAN = 9, 3, 3       # pulse span in symbols = PARTS windows of SPAN symbols
HISTORY = 8                    # symbols the kernels read before the first one
KAISER_BETA = 1.0              # one window for every profile; see report() for the margins
PEAK = 508                     # exact worst-case |I|, |Q| before rounding (10-bit limit 511)
COEF_FRAC, LEVEL_FRAC = 8, 30  # taps in 1/256 LSB, phase levels in 2^-30
SHIFT = COEF_FRAC + LEVEL_FRAC
LEVEL_I = np.round(np.cos(np.arange(8) * np.pi / 4) * 2 ** LEVEL_FRAC).astype(np.int64)
LEVEL_Q = np.round(np.sin(np.arange(8) * np.pi / 4) * 2 ** LEVEL_FRAC).astype(np.int64)

# EN 302 307-1 V1.4.1 Annex A, Table A.1 (p38): points (f / fN, dB) of the spectrum template,
# fN = Rs / 2. Upper: A C E G I J L P Q N S; lower: B D F H K M. Straight lines in dB between
# points as Figure A.1 draws them; the upper limit stays at -40 dB beyond S (assumed up to
# Fs / 2, where the DAC images begin); there is no lower limit beyond M. For roll-off 0.35 the
# annex refers to EN 300 421, whose template these points reproduce.
MASK = {
    0.20: dict(upper=[(0, .25), (.2, .25), (.4, .25), (.89, .15), (.94, -.5), (1, -2), (1.11, -8),
                      (1.23, -16), (1.4, -24), (1.5, -35), (1.7, -40)],
               lower=[(0, -.25), (.2, -.4), (.4, -.4), (.89, -1.1), (1, -4), (1.11, -11)]),
    0.35: dict(upper=[(0, .25), (.2, .25), (.4, .25), (.8, .15), (.9, -.5), (1, -2), (1.2, -8),
                      (1.4, -16), (1.6, -24), (1.8, -35), (2.12, -40)],
               lower=[(0, -.25), (.2, -.4), (.4, -.4), (.8, -1.1), (1, -4), (1.2, -11)]),
}


# ---------------------------------------------------------------- pulse

def rrc(t, alpha):
    """Root-raised-cosine impulse response, t in symbol periods (peak 1 - a + 4a/pi at t = 0)."""
    t = np.asarray(t, float)
    h = np.empty_like(t)
    x = 4 * alpha * t
    zero, sing = np.isclose(t, 0), np.isclose(np.abs(x), 1)
    r = ~(zero | sing)
    tr = t[r]
    h[r] = (np.sin(np.pi * tr * (1 - alpha)) + x[r] * np.cos(np.pi * tr * (1 + alpha))) / (
        np.pi * tr * (1 - x[r] ** 2))
    h[zero] = 1 - alpha + 4 * alpha / np.pi
    h[sing] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha))
                                    + (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
    return h


def window_rrc(alpha, n, span=L, beta=KAISER_BETA):
    """RRC truncated to span symbols (span * n taps, even, centre between two taps) times a
    Kaiser window."""
    k = np.arange(span * n)
    return rrc((k - (span * n - 1) / 2) / n, alpha) * np.kaiser(span * n, beta)


@dataclass(frozen=True)
class Profile:
    """A pulse and its table. alphabet '8psk': all eight phases, 3-bit codes (code = k).
    'qpsk': phases 1, 3, 5, 7, 2-bit codes (code = k >> 1), a table of 64 windows."""
    name: str
    alpha: float
    n: int
    alphabet: str

    @property
    def bits(self):
        return 3 if self.alphabet == "8psk" else 2

    @property
    def phases(self):
        return np.arange(8) if self.alphabet == "8psk" else np.array([1, 3, 5, 7])

    @property
    def windows(self):
        return 1 << (SPAN * self.bits)

    @property
    def block_words(self):
        """Parts 0, 1, 2 of one window, padded to a power of two so that an address is one
        shift and add."""
        return 1 << (PARTS * self.n - 1).bit_length()

    @property
    def table_bytes(self):
        return self.windows * self.block_words * 4

    @cached_property
    def taps(self):
        """Integer taps in 1/256 LSB, scaled so that the worst case over all symbol histories,
        max_p sum_a |g[aN + p]| * max|cos|, equals PEAK."""
        g = window_rrc(self.alpha, self.n)
        amax = np.abs(np.cos(self.phases * np.pi / 4)).max()
        worst = np.abs(g.reshape(L, self.n)).sum(axis=0).max() * amax
        return np.round(g * PEAK / worst * 2 ** COEF_FRAC).astype(np.int64)

    @cached_property
    def parts(self):
        """Rounded part values r[part, window, phase] for I and Q, before the bias."""
        w = np.arange(self.windows)
        codes = np.stack([(w >> (self.bits * i)) & ((1 << self.bits) - 1) for i in range(SPAN)])
        k = self.phases[codes]                                   # [age in window, window]
        out = []
        for level in (LEVEL_I, LEVEL_Q):
            c = self.taps.reshape(PARTS, SPAN, self.n)           # [part, age, phase p]
            s = np.einsum("aw,jap->jwp", level[k], c)            # exact in int64
            out.append((s + (1 << (SHIFT - 1))) >> SHIFT)        # floor(s / 2^SHIFT + 1/2)
        return out

    @cached_property
    def biases(self):
        """bias[part, phase]: parts 1 and 2 shifted to start at 0, part 0 takes the rest of 512."""
        out = []
        for r in self.parts:
            b = -r.min(axis=1)
            b[0] = 512 - b[1] - b[2]
            assert (r[0].min(axis=0) + b[0] >= 0).all(), "part 0 field below 0"
            assert ((r.max(axis=1) + b).sum(axis=0) <= 1023).all(), "field sum above 1023"
            out.append(b)
        return out

    @cached_property
    def table(self):
        """uint32 [windows, block_words]: parts 0, 1, 2 of each window, then zero padding."""
        (ri, rq), (bi, bq) = self.parts, self.biases
        words = (((ri + bi[:, None]) << I_SHIFT) | ((rq + bq[:, None]) << Q_SHIFT)).astype(np.uint32)
        t = np.zeros((self.windows, self.block_words), np.uint32)
        t[:, :PARTS * self.n] = words.transpose(1, 0, 2).reshape(self.windows, -1)
        return t


PROFILES = {p.name: p for p in (
    Profile("8psk-n4-a20", 0.20, 4, "8psk"),
    Profile("8psk-n4-a35", 0.35, 4, "8psk"),
    Profile("8psk-n8-a20", 0.20, 8, "8psk"),
    Profile("8psk-n8-a35", 0.35, 8, "8psk"),
    Profile("qpsk-n40-a20", 0.20, 40, "qpsk"),
    Profile("qpsk-n40-a35", 0.35, 40, "qpsk"),
)}
ON_DEVICE = ("8psk-n4-a20", "8psk-n4-a35", "8psk-n8-a20", "8psk-n8-a35", "qpsk-n40-a20", "qpsk-n40-a35")


# ---------------------------------------------------------------- symbols and samples

def qpsk_phase(b_i, b_q):
    """Phase index k of the QPSK point I = (1 - 2 b_i)/sqrt2, Q = (1 - 2 b_q)/sqrt2 (dvbs2.py)."""
    b_i, b_q = np.asarray(b_i, np.int64), np.asarray(b_q, np.int64)
    return 2 * (2 * b_q + (b_i ^ b_q)) + 1


def windows(p: Profile, sym):
    """Window index of every symbol from index 2 on: code[n] + code[n-1] << b + code[n-2] << 2b."""
    code = (np.asarray(sym, np.int64) >> 4) if p.bits == 3 else (np.asarray(sym, np.int64) >> 5)
    return code[2:] | code[1:-1] << p.bits | code[:-2] << 2 * p.bits


def shape(p: Profile, sym):
    """DAC words for sym[HISTORY:], as the firmware kernels make them. sym: bytes 16 * k with
    HISTORY symbols of history in front."""
    w = windows(p, sym)                   # w[i] is the window of symbol i + 2
    n0 = HISTORY - 2                      # index in w of the first output symbol
    t = p.table
    rows = (t[w[n0:], :p.n] + t[w[n0 - 3:len(w) - 3], p.n:2 * p.n]
            + t[w[n0 - 6:len(w) - 6], 2 * p.n:3 * p.n])
    return (rows ^ XOR).astype(np.uint32).ravel()


def iq(words):
    """DAC words -> complex samples in LSB (10-bit sign extension)."""
    w = np.asarray(words, np.int64)
    sx = lambda v: ((v & 0x3FF) ^ 0x200) - 0x200  # noqa: E731
    return sx(w >> I_SHIFT) + 1j * sx(w >> Q_SHIFT)


def linear(p: Profile, sym):
    """Exact filter output (no table rounding) for the same symbols, from the integer taps."""
    k = np.asarray(sym, np.int64) >> 4
    x = (LEVEL_I[k] + 1j * LEVEL_Q[k]) / 2 ** LEVEL_FRAC
    u = np.zeros(len(x) * p.n, complex)
    u[::p.n] = x
    y = np.convolve(u, p.taps / 2 ** COEF_FRAC)
    return y[HISTORY * p.n:len(x) * p.n]


def random_symbols(p: Profile, count, seed):
    """xorshift32 phases as membench makes them: x = xorshift(x); k = (x >> 16) & 7 (8psk) or
    2 * ((x >> 16) & 3) + 1 (qpsk); byte 16 * k. count includes the history."""
    out, x = np.empty(count, np.uint8), seed & 0xFFFFFFFF
    for i in range(count):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        k = (x >> 16) & 7 if p.alphabet == "8psk" else 2 * ((x >> 16) & 3) + 1
        out[i] = 16 * k
    return out


def checksum(words):
    """membench's checksum: h = (h ^ word) * 16777619 over 32-bit words, h0 = 2166136261."""
    h = 2166136261
    for v in np.asarray(words, np.uint32).tolist():
        h = ((h ^ v) * 16777619) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------- spectrum and MER

def response_db(taps, n, fn, hold=True):
    """20 log10 |G(f) / G(0)| of a symmetric FIR at f = fn * Rs / 2, times the DAC's zero-order
    hold sinc(f / Fs) if hold."""
    k = np.arange(len(taps)) - (len(taps) - 1) / 2
    g, fn = np.asarray(taps, float), np.atleast_1d(np.asarray(fn, float))
    h = np.concatenate([(np.cos(np.outer(f, k) * np.pi / n) * g).sum(axis=1)
                        for f in np.array_split(fn, 1 + len(fn) // 4096)])
    if hold:
        h = h * np.sinc(fn / (2 * n))
    return 20 * np.log10(np.abs(h) / abs(g.sum()) + 1e-300)


def mask_margins(taps, n, alpha, hold=True, step=1e-4):
    """Smallest distance (dB, positive = inside) of the pulse response to the Annex A template:
    upper limit up to S, lower limit up to M, and the -40 dB floor from S to Fs / 2. Grid of
    step * fN plus every template point."""
    up, lo = (np.array(MASK[alpha][k], float) for k in ("upper", "lower"))
    fn = np.unique(np.r_[np.arange(0, n + step / 2, step), up[:, 0], lo[:, 0]])
    db = response_db(taps, n, fn, hold)
    near = fn <= up[-1, 0]
    upper = np.interp(fn[near], up[:, 0], up[:, 1]) - db[near]
    m = fn <= lo[-1, 0]
    lower = db[m] - np.interp(fn[m], lo[:, 0], lo[:, 1])
    far = -40 - db[~near]
    return dict(upper=upper.min(), upper_at=fn[near][upper.argmin()], lower=lower.min(),
                lower_at=fn[m][lower.argmin()], far=far.min(), far_at=fn[~near][far.argmin()])


def rrc_response(f, alpha):
    """Ideal RRC amplitude, f in units of Rs."""
    f = np.abs(f)
    lo, hi = (1 - alpha) / 2, (1 + alpha) / 2
    h = np.where(f <= lo, 1.0, 0.0)
    m = (f > lo) & (f < hi)
    h[m] = np.sqrt(0.5 * (1 + np.cos(np.pi / alpha * (f[m] - lo))))
    return h


def receive(p: Profile, y, hold=True):
    """Symbol-rate output of an ideal RRC receiver for one period of a cyclic signal y
    (symbol i starts at sample i * N). Filter and hold act in the frequency domain; the pulse
    delay (LN - 1)/2 samples is removed exactly, the hold's half sample is left to timing."""
    f = np.fft.fftfreq(len(y)) * p.n                             # units of Rs
    h = rrc_response(f, p.alpha) * np.exp(2j * np.pi * f / p.n * (L * p.n - 1) / 2)
    if hold:
        h = h * np.sinc(f / p.n)
    return np.fft.ifft(np.fft.fft(y) * h)[::p.n]


def mer(p: Profile, y, sym, hold=True):
    """MER (dB) of cyclic output y for phase bytes sym, gain and phase fitted."""
    z = receive(p, y, hold)
    ref = np.exp(1j * np.pi * (np.asarray(sym, np.int64) >> 4) / 4)
    gain = np.vdot(ref, z) / np.vdot(ref, ref)
    return -10 * np.log10(np.mean(np.abs(z / gain - ref) ** 2))


def cyclic(p: Profile, count, seed):
    """Symbols for a periodic test signal: count symbols, with the last HISTORY copied in front."""
    s = random_symbols(p, count, seed)
    return np.r_[s[-HISTORY:], s]


def pulse_mer(p: Profile, hold=True):
    """MER limited by the pulse alone (exact taps, no table rounding): sum of the receiver
    output's symbol-spaced sidelobes, by numerical integration over the RRC band."""
    f = np.linspace(0, (1 + p.alpha) / 2, 20001)                 # units of Rs
    k = np.arange(len(p.taps)) - (len(p.taps) - 1) / 2
    a = (np.cos(2 * np.pi * np.outer(f, k) / p.n) * p.taps).sum(axis=1)
    if hold:
        a *= np.sinc(f / p.n)
    s = np.trapezoid(a * rrc_response(f, p.alpha) * np.cos(2 * np.pi * np.outer(np.arange(80), f)), f, axis=1)
    return -10 * np.log10(2 * (s[1:] ** 2).sum() / s[0] ** 2)


def image_dbc(p: Profile, step=1e-3):
    """Peak of the first DAC image (around Fs) relative to the response at 0 Hz, over the
    signal band |f| <= (1 + alpha) Rs / 2, with the ideal hold. The radio's analog filter is
    not included."""
    fn = np.arange(-(1 + p.alpha), 1 + p.alpha + step, step)       # f / fN about the image
    return (response_db(p.taps, p.n, np.abs(fn), hold=False)
            + 20 * np.log10(np.abs(np.sinc(1 + fn / (2 * p.n))))).max()


def report_profile(p: Profile, count=16384, seed=7):
    """Numbers for one profile from a cyclic random symbol sequence: mask margins, MER of the
    table output and its parts, rounding noise, levels, first image, table size."""
    s = cyclic(p, count, seed)
    y, exact = iq(shape(p, s)), linear(p, s)
    err = y - exact
    sym = s[HISTORY:]
    ref = np.exp(1j * np.pi * (sym.astype(np.int64) >> 4) / 4)
    gain = np.vdot(ref, receive(p, exact)) / np.vdot(ref, ref)
    # Rounding noise density against the signal's in-band density (|f| < 0.4 fN), both
    # averaged over bands of Rs / 32, worst band from template point S to Fs / 2.
    f = np.abs(np.fft.fftfreq(len(y)) * 2 * p.n)                 # f / fN
    band = (f * 16).astype(int)
    pe = np.bincount(band, np.abs(np.fft.fft(err)) ** 2) / np.bincount(band)
    inband = np.mean(np.abs(np.fft.fft(y)[f < 0.4]) ** 2)
    far = np.unique(band[(f >= MASK[p.alpha]["upper"][-1][0]) & (f <= p.n)])
    rms = np.sqrt(np.mean(np.abs(y) ** 2) / 2)
    peak = max(max(int((r.max(axis=1) + b).sum(axis=0).max()) - 512,
                   512 - int((r.min(axis=1) + b).sum(axis=0).min())) for r, b in zip(p.parts, p.biases))
    return dict(
        profile=p.name, alpha=p.alpha, samples_per_symbol=p.n, alphabet=p.alphabet,
        span_symbols=L, kaiser_beta=KAISER_BETA, taps=len(p.taps),
        mask_hold=mask_margins(p.taps, p.n, p.alpha), mask_digital=mask_margins(p.taps, p.n, p.alpha, False),
        mer_pulse_db=pulse_mer(p), mer_pulse_no_hold_db=pulse_mer(p, False),
        mer_table_db=mer(p, y, sym), mer_table_no_hold_db=mer(p, y, sym, False),
        mer_rounding_db=10 * np.log10(abs(gain) ** 2 / np.mean(np.abs(receive(p, err)) ** 2)),
        rounding_floor_db=10 * np.log10(pe[far].max() / inband),
        max_rounding_error_lsb=float(np.abs(np.r_[err.real, err.imag]).max()),
        rounding_rms_lsb=float(np.sqrt(np.mean(np.abs(err) ** 2) / 2)),
        rms_lsb=float(rms), rms_dbfs=float(20 * np.log10(rms / 512)), peak_lsb=peak,
        first_image_dbc=float(image_dbc(p)),
        windows=p.windows, block_bytes=4 * p.block_words, table_bytes=p.table_bytes,
        table_check=checksum(p.table.ravel()))


def formats(n, alphabet, span=L):
    """Bytes of the table formats considered, for a span of span symbols and n samples per
    symbol (4-byte DAC words), and PIE instructions per 4 output samples (adds fused into
    loads and stores). joint: one row per full history; per_axis: an I and a Q table of full
    per-axis histories combined with OR (8PSK rotated by pi/8 to 4 levels per axis); split:
    this design, three windows of three symbols (64-, 128- or 512-byte blocks)."""
    m = 8 if alphabet == "8psk" else 4
    levels = 4 if alphabet == "8psk" else 2
    p = Profile("", 0.2, n, alphabet)
    return dict(joint=m ** span * n * 4, per_axis=2 * levels ** span * n * 4,
                split=PARTS * p.windows * n * 4, split_padded=p.table_bytes,
                ops=dict(joint=2, per_axis=4, split=5))


# ---------------------------------------------------------------- firmware

# Device tests (firmware/main/membench.c): symbol counts that nearly fill a 16380-word bank.
TESTS = {"shape4": ("8psk-n4-a20", 4093), "shape4a35": ("8psk-n4-a35", 4093),
         "shape8": ("8psk-n8-a20", 2047), "shape40": ("qpsk-n40-a20", 409)}
SEED = 2026


def expected_checks():
    """Check values membench should report for the shaper tests."""
    out = {}
    for test, (name, count) in TESTS.items():
        p = PROFILES[name]
        words = shape(p, random_symbols(p, count + HISTORY, SEED))
        out[f"{test}_build"] = checksum(p.table.ravel())
        out[f"{test}_c"] = out[f"{test}_pie"] = checksum(words)
    return out


def header_text():
    lines = ["// Generated by host/shaper.py header. Do not edit.",
             f"// Pulse taps: RRC truncated to {L} symbols, Kaiser beta {KAISER_BETA}, in 1/256 LSB,",
             f"// scaled to a worst-case peak of {PEAK} LSB. Phase levels cos and sin(k pi/4) in 2^-30.",
             "#pragma once", "#include <stdint.h>", "",
             f"#define SHAPER_SHIFT {SHIFT}  // part sum >> SHIFT (rounded) gives LSB",
             f"#define SHAPER_I_SHIFT {I_SHIFT}  // DAC word fields",
             f"#define SHAPER_Q_SHIFT {Q_SHIFT}", "",
             "static const int32_t shaper_level_i[8] = {" + ", ".join(map(str, LEVEL_I)) + "};",
             "static const int32_t shaper_level_q[8] = {" + ", ".join(map(str, LEVEL_Q)) + "};"]
    for name in ON_DEVICE:
        p = PROFILES[name]
        ident = "shaper_taps_" + name.replace("-", "_")
        lines += ["", f"// {name}: alpha {p.alpha}, {p.n} samples/symbol, {p.alphabet}",
                  f"static const int32_t {ident}[{len(p.taps)}] = {{"]
        vals = [str(int(v)) for v in p.taps]
        lines += ["    " + ", ".join(vals[i:i + 12]) + "," for i in range(0, len(vals), 12)]
        lines.append("};")
    return "\n".join(lines) + "\n"


def verify(path):
    """Compare the shaper records in a membench JSON-lines file with expected_checks()."""
    want, bad, seen = expected_checks(), 0, set()
    for line in pathlib.Path(path).read_text().splitlines():
        r = json.loads(line)
        if r.get("test") in want:
            seen.add(r["test"])
            ok = r["check"] == want[r["test"]]
            bad += not ok
            print(f"{r['test']:14s} board {r['check']:10d} model {want[r['test']]:10d} "
                  f"{'same' if ok else 'DIFFERENT'}")
    missing = sorted(set(want) - seen)
    if missing:
        print("missing:", " ".join(missing))
    return 1 if bad or missing else 0


def report():
    for p in PROFILES.values():
        r = report_profile(p)
        mh, md = r["mask_hold"], r["mask_digital"]
        print(f"{p.name}: {r['taps']} taps, table {r['windows']} x {r['block_bytes']} B = "
              f"{r['table_bytes'] // 1024} KiB, check {r['table_check']}")
        for label, m in (("hold", mh), ("no hold", md)):
            print(f"  mask {label:7s}: upper {m['upper']:+.3f} dB at {m['upper_at']:.4f} fN, "
                  f"lower {m['lower']:+.3f} dB at {m['lower_at']:.4f} fN, "
                  f"floor {m['far']:+.3f} dB at {m['far_at']:.3f} fN")
        print(f"  MER: table {r['mer_table_db']:.2f} dB (no hold {r['mer_table_no_hold_db']:.2f}); "
              f"pulse alone {r['mer_pulse_db']:.2f} dB (no hold {r['mer_pulse_no_hold_db']:.2f}); "
              f"rounding alone {r['mer_rounding_db']:.1f} dB")
        print(f"  rounding: max {r['max_rounding_error_lsb']:.3f} LSB, rms {r['rounding_rms_lsb']:.3f}"
              f" LSB, floor {r['rounding_floor_db']:.1f} dB below in-band density; level rms "
              f"{r['rms_lsb']:.1f} LSB ({r['rms_dbfs']:.1f} dBFS), peak {r['peak_lsb']}; first "
              f"image {r['first_image_dbc']:.1f} dBc")
    print("table bytes for a 9-symbol span (joint rows / per-axis I and Q with OR / split sums,"
          " packed and padded) and PIE instructions per 4 samples:")
    for n, alphabet in ((4, "8psk"), (8, "8psk"), (8, "qpsk"), (40, "qpsk"), (40, "8psk")):
        b = formats(n, alphabet)
        print(f"  {alphabet} N={n:2d}: {b['joint']:>13,d} / {b['per_axis']:>11,d} / "
              f"{b['split']:>7,d} ({b['split_padded']:,d}); ops {b['ops']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=["report", "header", "verify"])
    ap.add_argument("runs", nargs="?", help="membench JSON-lines file (verify)")
    a = ap.parse_args()
    if a.command == "header":
        HEADER.write_text(header_text())
        print(f"wrote {HEADER.relative_to(ROOT)}")
    elif a.command == "verify":
        return verify(a.runs)
    else:
        report()
    return 0


if __name__ == "__main__":
    sys.exit(main())
