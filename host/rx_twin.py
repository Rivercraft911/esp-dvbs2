#!/usr/bin/env python3
"""Digital twin of the S3-to-S3 link: transmitter frames in, receiver dump words out.

    python3 host/rx_twin.py --frames 40 --mer 18 --out twin.u32     write dump words
    python3 host/rx_twin.py --frames 40 --check                     run the C demodulator on it

Transmitter: host/dvbs2.py frames carrying the firmware's test payload (host/rx.py model), the
firmware pulse (shaper_taps.h: inverse-hold RRC, 360 taps at 40 MS/s), each DAC sample held
for 25 ns, then band-limited to the dump's +-8 MHz. Channel and receiver: symbol clock offset
and carrier offset that drift together (both radios run from 40 MHz crystals), Wiener phase
noise, the transmitter's carrier leak and IQ image, the receiver's IQ image and DC, AWGN set
by Es/N0 at an ideal matched filter, level, 10-bit rounding, and junk in the unused word bits.
An optional gap replaces frames with a keyed +1 MHz tone, as the station ID does.

Defaults are the coax link as measured (2026-10-20 and 2026-10-22 captures): CFO -16.6 kHz
drifting up to -260 Hz/s, leak -17 dB, receiver DC -23 dB and image -38 dB, rms 221 LSB at
RX gain 32. Every random draw comes from one seeded generator, so a seed gives one file.
"""
import argparse
import dataclasses
import functools
import pathlib
import re
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2  # noqa: E402
import rx  # noqa: E402

FS = 16e6                    # dump sample rate
RS = 1e6                     # symbol rate
GRID = 2048                  # pulse table points per symbol


@dataclasses.dataclass
class Link:
    modcod: int = 7
    short: bool = True
    frames: int = 20
    first: int = 0               # first payload frame of the transmitter model
    callsign: str = "KJ5SCH"
    carrier_hz: float = 2420e6
    cfo_hz: float = -16600.0
    drift_hz_s: float = -260.0
    clock_ppm: float = None      # TX symbol clock vs RX sample clock; None: cfo / carrier
    mer_db: float = 18.0         # AWGN Es/N0 at an ideal matched filter
    linewidth_hz: float = 0.05   # Wiener phase noise: 1.6 deg rms about a line over 33 ms
    rms: float = 221.0           # 10-bit LSB before noise
    rx_dc_db: float = -23.0
    rx_image_db: float = -38.0
    tx_leak_db: float = -17.0
    tx_image_db: float = -32.0
    gap: tuple = None            # (after this many frames, seconds of keyed tone)
    lead_symbols: int = 500      # receiver noise before the transmitter starts
    seed: int = 2026


@functools.lru_cache(maxsize=1)
def pulse():
    """Transmitted pulse on a 1/GRID-symbol grid from t = -2 to 14 symbols: the firmware taps,
    each DAC sample held for 1/40 symbol, band-limited to +-8 MHz, scaled so a stream of unit
    symbols has unit rms (sum of p^2 at 16 samples/symbol = 16)."""
    text = (ROOT / "firmware/main/shaper_taps.h").read_text()
    taps = np.array([int(v) for v in re.search(r"qpsk_n40_a35\[360\] = \{([^}]*)\}", text)
                     .group(1).split(",") if v.strip()], float)
    n = 16 * GRID
    t = np.arange(n) / GRID - 2
    k = np.floor(t * 40).astype(int)                    # DAC sample being held
    held = np.where((k >= 0) & (k < len(taps)), taps[np.clip(k, 0, len(taps) - 1)], 0.0)
    f = np.fft.fftfreq(n, 1 / (GRID * RS))
    p = np.real(np.fft.ifft(np.fft.fft(held) * (np.abs(f) < FS / 2)))
    return p * np.sqrt(16 / np.sum(p[::GRID // 16] ** 2))


def frames(link):
    """Symbols (phase index, 0..7) and per-frame truth: first symbol, model frame, codeword."""
    rate = dvbs2.RATES[link.modcod - 1]
    size = (dvbs2.code(rate, link.short).kbch - 80) // 8
    model = rx.model(size, link.first + link.frames, link.callsign)
    parts, truth, at = [np.zeros(link.lead_symbols, np.uint8)], [], link.lead_symbols
    active = [np.zeros(link.lead_symbols, bool)]
    for j in range(link.frames):
        if link.gap and j == link.gap[0]:
            g = int(round(link.gap[1] * RS))
            parts.append(np.zeros(g, np.uint8))
            active.append(np.zeros(g, bool))
            at += g
        k = link.first + j
        bits = np.unpackbits(np.frombuffer(model[k], np.uint8))
        cw = dvbs2.fecframe(dvbs2.bbframe(bits, rate, link.short), rate, link.short)
        ph = dvbs2.plframe_phases(cw, rate, link.short)
        truth.append(dict(symbol=at, model_frame=k, codeword=cw, phases=ph))
        parts.append(ph)
        active.append(np.ones(len(ph), bool))
        at += len(ph)
    tail = 3000                       # receiver keeps running after the last frame
    parts.append(np.zeros(tail, np.uint8))
    active.append(np.zeros(tail, bool))
    return np.concatenate(parts), np.concatenate(active), truth


def clock(link):
    """Sample n's transmitter time in symbols: (n + e0 n + de n^2 / 2) / 16."""
    if link.clock_ppm is None:
        return link.cfo_hz / link.carrier_hz, link.drift_hz_s / (link.carrier_hz * FS)
    return link.clock_ppm * 1e-6, 0.0


def generate(link, chunk=1 << 18):
    """Yield (first sample, dump words) chunks, and finally the truth list."""
    rng = np.random.default_rng(link.seed)
    table = pulse()
    sym, act, truth = frames(link)
    a = np.where(act, np.exp(1j * np.pi / 4 * sym), 0)
    e0, de = clock(link)
    total = int(len(sym) * 16 / (1 + e0)) - 16
    db = lambda v: 10 ** (v / 20)                                   # noqa: E731
    leak = db(link.tx_leak_db) * np.exp(0.7j)
    tx_nu, rx_nu = db(link.tx_image_db) * np.exp(0.3j), db(link.rx_image_db) * np.exp(-1.1j)
    dc = link.rms * db(link.rx_dc_db) * np.exp(2.1j)
    # A matched filter sums 16 samples of signal coherently: Es/N0 = 16 rms^2 / sigma^2.
    sigma = 4 * link.rms / np.sqrt(10 ** (link.mer_db / 10))
    pn_step = np.sqrt(2 * np.pi * link.linewidth_hz / FS)
    pn = 0.0
    for first in range(0, total, chunk):
        n = np.arange(first, min(first + chunk, total), dtype=np.float64)
        t = (n + e0 * n + de * n * n / 2) / 16                      # symbols, TX clock
        k0 = np.floor(t).astype(np.int64)
        x = np.zeros(len(n), complex)
        for j in range(-2, 14):                                     # pulse spans t - k in [-2, 14)
            k = k0 - j
            tau = (t - k + 2) * GRID
            i = np.clip(tau.astype(np.int64), 0, len(table) - 2)
            w = tau - i
            p = table[i] * (1 - w) + table[i + 1] * w
            x += np.where((k >= 0) & (k < len(a)), a[np.clip(k, 0, len(a) - 1)], 0) * p
        tx_on = act[np.clip(k0, 0, len(act) - 1)]
        x = x + tx_nu * np.conj(x) + leak * tx_on
        tone = ~tx_on & (k0 >= link.lead_symbols) & (k0 < len(sym) - 3000)
        if tone.any():                                              # keyed station-ID tone
            key = (np.floor(n / FS / 0.06) % 4) != 3
            x += np.where(tone & key, np.exp(2j * np.pi * 1e6 * n / FS), 0)
        steps = rng.normal(0, pn_step, len(n))
        pn_t = pn + np.cumsum(steps)
        pn = pn_t[-1]
        s = n / FS
        x *= link.rms * np.exp(1j * (2 * np.pi * (link.cfo_hz * s + link.drift_hz_s * s * s / 2) + pn_t))
        x = x + rx_nu * np.conj(x) + dc
        x += sigma / np.sqrt(2) * (rng.normal(size=len(n)) + 1j * rng.normal(size=len(n)))
        i_ = np.clip(np.round(x.real), -512, 511).astype(np.int64)
        q_ = np.clip(np.round(x.imag), -512, 511).astype(np.int64)
        junk = rng.integers(0, 4096, len(n), dtype=np.int64) << 20
        yield first, ((i_ & 1023) << 10 | (q_ & 1023) | junk).astype(np.uint32)
    for f in truth:                                                 # approximate, for matching
        f["header_sample"] = (f["symbol"] + 4.5) * 16 / (1 + e0)
    yield None, truth


def reference(words, truth, link):
    """Float reference receiver with the truth: the full 10-bit samples less the true DC, a
    10-symbol RRC matched filter at the true symbol times, the true carrier offset and drift
    removed, the remaining phase from a 101-symbol moving average of known symbols, amplitude
    and carrier leak fitted on known symbols.
    Returns each frame's data-symbol MER in dB, the yardstick for implementation loss."""
    from rx_demod import rrc
    x = (((words.astype(np.int64) << 12) & 0xffffffff) ^ 0x80000000) - 0x80000000 >> 22
    y = (((words.astype(np.int64) << 22) & 0xffffffff) ^ 0x80000000) - 0x80000000 >> 22
    x = x + 1j * y - link.rms * 10 ** (link.rx_dc_db / 20) * np.exp(2.1j)
    table = pulse()
    t = np.arange(len(table)) / GRID - 2
    g = rrc(np.arange(-80, 81) / 16)
    g /= np.sqrt(np.sum(g ** 2))
    centre = t[np.argmax(np.convolve(table, rrc(np.arange(-5 * GRID, 5 * GRID + 1) / GRID), "same"))]
    e0, de = clock(link)
    out = []
    for f in truth:
        k = np.arange(len(f["phases"]))
        n = 16 * (f["symbol"] + k + centre) / (1 + e0)
        n -= de * n * n / 2 / (1 + e0)
        m0 = np.floor(n).astype(np.int64)
        idx = m0[:, None] + np.arange(-80, 82)[None, :]
        if idx.min() < 0 or idx.max() >= len(x):
            out.append(None)
            continue
        h = rrc((n[:, None] - idx) / 16) / np.sqrt(np.sum(rrc(np.arange(-80, 81) / 16) ** 2))
        sec = n / FS
        ym = np.sum(x[idx] * h, axis=1) * np.exp(-2j * np.pi * (link.cfo_hz * sec + link.drift_hz_s * sec * sec / 2))
        s = np.exp(1j * np.pi / 4 * f["phases"])
        ph = np.angle(np.convolve(ym * np.conj(s), np.ones(101), "same"))
        yd = ym * np.exp(-1j * ph)
        a = np.stack([s, np.ones(len(s))], 1)
        w = np.linalg.lstsq(a, yd, rcond=None)[0]
        err = np.abs((yd - w[1]) / w[0] - s) ** 2
        data = np.ones(len(s), bool)
        data[:90] = False
        for j in range(1, (len(s) - 90) // 1476 + 1):
            data[90 + 1476 * j - 36:90 + 1476 * j] = False
        out.append(-10 * np.log10(np.mean(err[data])))
    return out


def twin(link):
    """All dump words and the truth list."""
    words, truth = [], None
    for first, w in generate(link):
        if first is None:
            truth = w
        else:
            words.append(w)
    return np.concatenate(words), truth


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    for f in dataclasses.fields(Link):
        if f.type in (int, float, str):
            p.add_argument("--" + f.name.replace("_", "-"), type=f.type, default=f.default)
    p.add_argument("--normal", action="store_true")
    p.add_argument("--out", type=pathlib.Path)
    a = p.parse_args()
    link = Link(**{f.name: getattr(a, f.name) for f in dataclasses.fields(Link) if hasattr(a, f.name)},
                short=not a.normal)
    words, truth = twin(link)
    if a.out:
        words.tofile(a.out)
    print(f"{len(words)} words, {len(truth)} frames")


if __name__ == "__main__":
    main()
