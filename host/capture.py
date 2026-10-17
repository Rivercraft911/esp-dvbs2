#!/usr/bin/env python3
"""Measure raw interleaved little-endian I/Q captures. No SDR or board is opened.

    python3 host/capture.py tone.iq --format i16 --rate 61440000 --signal tone
    python3 host/capture.py tag.iq --format f32 --rate 61440000 --signal tag
    python3 host/capture.py zero.iq --format i16 --rate 4000000 --signal lines
    python3 host/capture.py gap.iq --format i16 --rate 61440000 --signal gap --scale 2048

Tone offsets are relative to the SDR capture center; tune it to the requested
TX frequency. The tone search allows +/-100 kHz of carrier error. The board's
carrier is taken as the first tone minus 1 MHz (a DAC clock error of 50 ppm moves
that tone by only 50 Hz), and the image and LO leak are measured around it, not
around the SDR center. Each line is integrated over +/-1 kHz so carrier drift
during the capture stays inside.
Float samples use full scale 1; int16 samples default to 32768 (--scale 2048 for E200). Power is
mean(|I+jQ|**2), in dBFS, never dBm. The SDR sample-rate argument is the time
reference, so inferred DAC rate inherits its calibration error.

Tag protocol (independently designed here): state starts at 1; output +256+j0
when state bit 0 is 1, otherwise -256+j0. Advance once per DAC sample with
s = (s >> 1) ^ ((0 - (s & 1)) & 0x80200003). Any 32 consecutive phase bits
identify the state. Its 2**32-1 sample period is checked algebraically in tests,
not assumed from a polynomial table. At 40 MS/s the period is 107.374182375 s.
The firmware limits a tag run to 60 s. It does not repeat a bank-sized waveform.

A tag capture needs at least 50 MS/s, a receiver response covering the 40 MHz
Nyquist band, a clean BPSK eye and modest clock error (<250 ppm). A narrowband
capture cannot prove continuity of individual 25 ns DAC samples. The quality
gates below reject acquisition failures; passing synthetic tests does not
qualify an unknown analog receiver/filter. Reported discontinuities are in
source samples. Their location is ambiguous within runs of equal phase bits.
Jump direction uses the shortest signed modulo-period displacement, so an
isolated jump of >=2**31 samples cannot be classified uniquely. A nonzero
jump still fails continuity; bank-sized faults are far below that limit.
"""
from __future__ import annotations

import argparse
import functools
import json
import math
import pathlib
import sys

import numpy as np

DAC_RATE = 40_000_000.0
TAG_MASK = 0x80200003
TAG_PERIOD = (1 << 32) - 1


def tag_step(state: int) -> int:
    return (state >> 1) ^ ((-(state & 1)) & TAG_MASK)


def _apply(columns: list[int], state: int) -> int:
    result = 0
    while state:
        bit = state & -state
        result ^= columns[bit.bit_length() - 1]
        state ^= bit
    return result


@functools.lru_cache(maxsize=128)
def _jump_columns(count: int) -> tuple[int, ...]:
    count %= TAG_PERIOD
    columns = [tag_step(1 << i) for i in range(32)]
    result = [1 << i for i in range(32)]
    while count:
        if count & 1:
            result = [_apply(columns, x) for x in result]
        columns = [_apply(columns, x) for x in columns]
        count >>= 1
    return tuple(result)


def tag_jump(state: int, count: int) -> int:
    return _apply(_jump_columns(count), state)


def tag_bits(first: int, count: int) -> np.ndarray:
    """Exact firmware reference, including arbitrary bank-overlap start indices."""
    state = tag_jump(1, first)
    result = np.empty(count, dtype=np.uint8)
    for i in range(count):
        result[i] = state & 1
        state = tag_step(state)
    return result


@functools.lru_cache(maxsize=1)
def _observation_inverse() -> tuple[tuple[int, ...], int]:
    # Row k describes output bit k as a linear function of the initial state.
    columns = [1 << i for i in range(32)]
    rows = []
    for k in range(32):
        rows.append(sum((x & 1) << i for i, x in enumerate(columns)) | (1 << (32 + k)))
        columns = [tag_step(x) for x in columns]
    for i in range(32):
        pivot = next(k for k in range(i, 32) if (rows[k] >> i) & 1)
        rows[i], rows[pivot] = rows[pivot], rows[i]
        for k in range(32):
            if k != i and ((rows[k] >> i) & 1):
                rows[k] ^= rows[i]
    inverse = tuple(x >> 32 for x in rows)
    feedback = 0
    for i, x in enumerate(columns):
        if x & 1:
            feedback ^= inverse[i]
    return inverse, feedback


def _state(word: int) -> int:
    return sum(((row & word).bit_count() & 1) << i
               for i, row in enumerate(_observation_inverse()[0]))


def _words(bits: np.ndarray) -> np.ndarray:
    result = np.zeros(max(0, len(bits) - 31), dtype=np.uint32)
    for i in range(32):
        result ^= bits[i:i + len(result)].astype(np.uint32) << i
    return result


def _parity(x: np.ndarray) -> np.ndarray:
    x = x.copy()
    for shift in (16, 8, 4, 2, 1):
        x ^= x >> shift
    return (x & 1).astype(np.uint8)


def _checks(bits: np.ndarray, words: np.ndarray | None = None) -> np.ndarray:
    if words is None:
        words = _words(bits)
    return _parity(words[:-1] & _observation_inverse()[1]) ^ bits[32:]


class TagIndex:
    """Baby-step/giant-step discrete log of the 32-bit linear state transition."""
    def __init__(self):
        self.baby = {}
        state = 1
        for i in range(65536):
            self.baby[state] = i
            state = tag_step(state)
        columns = _jump_columns(-65536)
        self.tables = [[_apply(columns, x << (8 * byte)) for x in range(256)]
                       for byte in range(4)]

    def index(self, state: int) -> int:
        if state == 0:
            raise ValueError("zero is not a transmitted tag state")
        for giant in range(65536):
            if state in self.baby:
                return (65536 * giant + self.baby[state]) % TAG_PERIOD
            state = (self.tables[0][state & 255] ^ self.tables[1][(state >> 8) & 255]
                     ^ self.tables[2][(state >> 16) & 255] ^ self.tables[3][state >> 24])
        raise ValueError("tag state has no counter index")


class Capture:
    def __init__(self, path: pathlib.Path, fmt: str, scale: float | None = None):
        dtype = {"i16": "<i2", "f32": "<f4"}[fmt]
        size = np.dtype(dtype).itemsize
        if path.stat().st_size % (2 * size):
            raise ValueError("capture ends in a partial I/Q sample")
        self.raw = np.memmap(path, mode="r", dtype=dtype).reshape(-1, 2)
        self.scale = scale if scale is not None else (32768.0 if fmt == "i16" else 1.0)
        if not math.isfinite(self.scale) or self.scale <= 0:
            raise ValueError("full scale must be positive and finite")
        if len(self.raw) < 256:
            raise ValueError("capture needs at least 256 complex samples")

    def __len__(self):
        return len(self.raw)

    def read(self, first: int, last: int) -> np.ndarray:
        block = self.raw[first:last].astype(np.float64) / self.scale
        if not np.isfinite(block).all():
            raise ValueError("capture contains non-finite samples")
        return block[:, 0] + 1j * block[:, 1]


def _db(power: float) -> float | None:
    return 10 * math.log10(power) if power > 0 else None


def spectrum(capture, rate: float, signal: str) -> dict:
    """Welch spectrum: integrated powers, Hann window and 50% overlap."""
    nfft = min(262144, 1 << (len(capture).bit_length() - 1))
    window = np.hanning(nfft)
    psd = np.zeros(nfft)
    power_sum, used, count = 0.0, 0, 0
    for start in range(0, len(capture) - nfft + 1, nfft // 2):
        z = capture.read(start, start + nfft)
        power_sum += float(np.vdot(z, z).real)
        used += len(z)
        psd += abs(np.fft.fftshift(np.fft.fft(z * window))) ** 2
        count += 1
    if power_sum <= 0:
        raise ValueError("capture has zero power")
    psd /= count * nfft * float(np.dot(window, window))
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate))
    resolution = rate / nfft
    expected = [1e6] if signal == "tone" else [1e6, 3e6]
    width = max(4 * resolution, 1000.0)
    if any(abs(f) + 100000 >= rate / 2 for f in expected):
        raise ValueError("sample rate does not cover the expected tones")
    def peak_offset(peak):
        if peak == 0 or peak == nfft - 1:
            return float(freqs[peak])
        logs = np.log(np.maximum(psd[peak - 1:peak + 2], 1e-300))
        divisor = logs[0] - 2 * logs[1] + logs[2]
        shift = .5 * (logs[0] - logs[2]) / divisor if divisor else 0
        return float(freqs[peak] + np.clip(shift, -.5, .5) * resolution)

    dominant_peak = int(np.argmax(psd))
    dominant_offset = peak_offset(dominant_peak)
    excluded = np.zeros(nfft, dtype=bool)
    tones, carrier = [], 0.0
    for f in expected:
        region = np.flatnonzero(abs(freqs - carrier - f) < 100000)
        peak = int(region[np.argmax(psd[region])])
        if psd[peak] < psd[dominant_peak] * .001:
            raise ValueError(f"expected {f:g} Hz tone absent; dominant component is {dominant_offset:g} Hz")
        offset = peak_offset(peak)
        if not tones:
            carrier = offset - f
        mask = abs(freqs - offset) <= width
        excluded |= mask
        tone_power = float(psd[mask].sum())
        image_mask = abs(freqs - (2 * carrier - offset)) <= width
        image = float(psd[image_mask].sum())
        tones.append({"offset_hz": offset, "power_dbfs": _db(tone_power),
                      "image_dbc": _db(image / tone_power)})
    reference = max(10 ** (x["power_dbfs"] / 10) for x in tones)
    dc_mask = abs(freqs) <= width
    dc = float(psd[dc_mask].sum())
    lo_mask = abs(freqs - carrier) <= width
    lo = float(psd[lo_mask].sum())
    excluded |= dc_mask | lo_mask
    # A spur is integrated over the same 9-bin width as a carrier, not one bin.
    integrated = np.convolve(psd, np.ones(9), mode="same")
    excluded = np.convolve(excluded.astype(np.int8), np.ones(9), mode="same") > 0
    peak = int(np.argmax(np.where(excluded, -1, integrated)))
    return {"signal": signal, "samples": len(capture), "sample_rate_hz": rate,
            "total_power_dbfs": _db(power_sum / used), "fft_bin_hz": resolution,
            "dominant_offset_hz": dominant_offset, "carrier_offset_hz": carrier,
            "integration_hz": width,
            "tones": tones, "lo_dbc": _db(lo / reference), "dc_dbc": _db(dc / reference),
            "strongest_spur_hz": float(freqs[peak]),
            "strongest_spur_dbc": _db(float(integrated[peak]) / reference)}


def modulation(capture, rate: float, offset: float, limit: int = 1 << 22) -> dict:
    """Envelope (AM) and phase (PM) wander of one tone: shift it to 0 Hz, keep +-60 kHz,
    remove a quadratic phase trend (frequency drift), then report rms values and the
    three strongest AM lines between 200 Hz and 60 kHz. Uses the first `limit` samples."""
    z = capture.read(0, min(len(capture), limit))
    n = len(z)
    t = np.arange(n) / rate
    spectrum_ = np.fft.fft(z * np.exp(-2j * np.pi * offset * t))
    freqs = np.fft.fftfreq(n, 1 / rate)
    spectrum_[abs(freqs) > 60000] = 0
    tone = np.fft.ifft(spectrum_)
    envelope = abs(tone)
    if envelope.mean() <= 0:
        raise ValueError("tone has zero envelope")
    phase = np.unwrap(np.angle(tone))
    k = np.arange(n)
    phase -= np.polyval(np.polyfit(k, phase, 2), k)
    am = envelope / envelope.mean() - 1
    lines = abs(np.fft.rfft(am * np.hanning(n))) ** 2
    line_freqs = np.fft.rfftfreq(n, 1 / rate)
    band = np.flatnonzero((line_freqs > 200) & (line_freqs < 60000))
    top = band[np.argsort(lines[band])[-3:][::-1]]
    return {"am_rms_pct": float(100 * am.std()), "pm_rms_deg": float(np.degrees(phase.std())),
            "am_lines_hz": [float(line_freqs[i]) for i in top]}


def lines(capture, rate: float, block: float = 0.01) -> dict:
    """Amplitudes at the board carrier and carrier +-1, +-2 MHz, for the diagnostic signals.

    The carrier is placed from the strongest line within 100 kHz of 0, +1 MHz or -1 MHz
    (minus that MHz). Each amplitude is the median over `block`-second coherent blocks of
    the best match within +-300 Hz, so drift does not smear it. Full scale is 1.
    """
    z = capture.read(0, len(capture))
    n = len(z)
    spectrum_ = abs(np.fft.fft(z * np.hanning(n)))
    freqs = np.fft.fftfreq(n, 1 / rate)
    best = None
    for k in (0, 1, -1):
        region = np.flatnonzero(abs(freqs - k * 1e6) < 100000)
        peak = region[np.argmax(spectrum_[region])]
        if best is None or spectrum_[peak] > best[0]:
            best = (spectrum_[peak], freqs[peak] - k * 1e6)
    carrier = float(best[1])
    size = max(256, int(block * rate))
    t = np.arange(n) / rate
    out = {"carrier_offset_hz": carrier, "block_s": size / rate}
    for k in (-2, -1, 0, 1, 2):
        f = carrier + k * 1e6
        if abs(f) >= rate / 2 - 100000:
            continue
        amps = []
        for start in range(0, n - size + 1, size):
            seg, tt = z[start:start + size], t[start:start + size]
            amps.append(max(abs(np.mean(seg * np.exp(-2j * np.pi * (f + d) * tt)))
                            for d in range(-300, 301, 25)))
        out[f"{k:+d}MHz"] = float(np.median(amps))
    return out


def gaps(capture, rate: float, dac_rate: float = DAC_RATE, step: int = 14268,
         lead: int = 64, tail: int = 2048) -> dict:
    """The switch probe (`tx ... gap`, radio_s3_set_gaps): a steady tone whose banks are silent
    in their own lead and tail, so each bank switch leaves |switch - handoff| silent DAC samples.
    The firmware aims each switch lead/2 early, so a gap of lead/2 + d says the DAC switched d
    samples after the CPU's model; any long gap is a late switch. Gaps are runs below half the
    median envelope, in DAC samples; runs longer than the overlap (CW ID spaces) are skipped.
    Defaults: the 40 MS/s geometry (radio_s3.h)."""
    z = capture.read(0, len(capture))
    envelope = np.abs(z)
    level = float(np.median(envelope))
    if level <= 0:
        raise ValueError("capture has no tone")
    low = np.concatenate(([False], envelope < level / 2, [False]))
    edges = np.flatnonzero(np.diff(low.astype(np.int8)))
    starts, ends = edges[::2], edges[1::2]
    scale = dac_rate / rate
    lengths = (ends - starts) * scale
    keep = lengths <= lead + tail
    lengths, centers = lengths[keep], (starts[keep] + ends[keep]) / 2 * scale
    if len(lengths) < 2:
        raise ValueError("fewer than two switch gaps found")
    spacing = np.diff(centers)
    # Switches inside one tone segment are STEP apart; an ID space can hide some.
    banks = np.rint(spacing / step)
    regular = spacing[banks == 1]
    pct = lambda x, q: float(np.percentile(x, q))
    return {"signal": "gap", "sample_rate_hz": rate, "dac_rate_hz": dac_rate,
            "step_samples": step, "aim_samples": lead / 2, "gaps": int(len(lengths)),
            "gap_samples": {"min": float(lengths.min()), "p1": pct(lengths, 1),
                            "median": float(np.median(lengths)), "p99": pct(lengths, 99),
                            "max": float(lengths.max())},
            "lag_samples_median": float(np.median(lengths)) - lead / 2,
            "spacing_samples": {"count": int(len(regular)),
                                "min": float(regular.min()) if len(regular) else None,
                                "max": float(regular.max()) if len(regular) else None},
            "off_grid": int(np.count_nonzero(abs(spacing - banks * step) > 4))}


def _carrier(z: np.ndarray) -> tuple[float, float]:
    """BPSK squaring removes the phase bits; fit the remaining carrier line."""
    squared = z * z
    coarse = .5 * np.angle(np.vdot(squared[:-1], squared[1:]))
    t = np.arange(len(z))
    residual = squared * np.exp(-2j * coarse * t)
    width = 256
    means = np.array([residual[i:i + width].mean() for i in range(0, len(z) - width, width)])
    times = np.arange(len(means)) * width + (width - 1) / 2
    phase = np.unwrap(np.angle(means))
    slope, intercept = np.polyfit(times, phase, 1, w=np.maximum(abs(means), 1e-12))
    return coarse + slope / 2, intercept / 2


def _sample(real: np.ndarray, q: float, phase: float) -> tuple[np.ndarray, np.ndarray]:
    # q is DAC samples / ADC sample; phase is in DAC samples. Skip filter edges.
    ticks = np.arange(math.ceil(8 * q + phase), math.floor((len(real) - 9) * q + phase))
    times = (ticks - phase) / q
    return np.interp(times, np.arange(len(real)), real), times


def _eye_cost(real: np.ndarray, q: float, phase: float) -> float:
    samples, _ = _sample(real, q, phase)
    square = samples * samples
    mean = square.mean()
    return float(np.mean(square * square) / max(mean * mean, 1e-30) - 1)


def _timing(real: np.ndarray, q: float, phase: float | None) -> tuple[float, float, float]:
    # Initial 16k-sample block: coarse joint search, then local refinements.
    if phase is None:
        best = (float("inf"), q, 0.0)
        for trial_q in q * (1 + np.linspace(-250e-6, 250e-6, 21)):
            for trial_phase in np.arange(0, 1, 1 / 16):
                cost = _eye_cost(real, trial_q, trial_phase)
                if cost < best[0]:
                    best = cost, trial_q, trial_phase
        cost, q, phase = best
        for ppm, step in ((25e-6, 1 / 16), (5e-6, 1 / 64)):
            best = min((_eye_cost(real, tq, tp), tq, tp)
                       for tq in q * (1 + np.linspace(-ppm, ppm, 5))
                       for tp in phase + np.linspace(-step, step, 5))
            cost, q, phase = best
        return q, phase, cost
    # Track phase only. The unwrapped phase records the receive-clock drift.
    best = min((_eye_cost(real, q, tp), tp)
               for tp in phase + np.linspace(-.4, .4, 17))
    cost, phase = best
    best = min((_eye_cost(real, q, tp), tp)
               for tp in phase + np.linspace(-.025, .025, 5))
    return q, best[1], best[0]


class TagVerifier:
    """Verify every recovered sample with 63 bits carried across input blocks."""
    def __init__(self):
        self.total = 0
        self.tail = np.empty(0, np.uint8)
        self.index = None
        self.previous_position = self.previous_counter = self.previous_state = None
        self.first_counter = self.checked_start = self.checked_stop = None
        self.errors = self.good_anchors = self.unexplained = 0
        self.events = []

    def feed(self, new_bits: np.ndarray) -> None:
        bits = np.r_[self.tail, new_bits]
        base = self.total - len(self.tail)
        checks = _checks(bits)
        # With 63 carried bits, 31 recurrence checks were counted previously.
        first_new_check = max(0, self.total - 32 - base)
        self.errors += int(checks[first_new_check:].sum())
        self.total += len(new_bits)
        self.tail = bits[-63:].copy()
        if len(checks) < 32:
            return
        # Each anchor needs 64 consistent bits. A random wrong window passes
        # with probability 2^-32 before the further neighboring checks.
        boundaries = np.diff(np.r_[False, checks == 0, False].astype(np.int8))
        starts, stops = np.flatnonzero(boundaries == 1), np.flatnonzero(boundaries == -1)
        enough = stops - starts >= 32
        starts, stops = starts[enough], stops[enough] - 31
        self.good_anchors += int((stops - starts).sum())
        for start, stop in zip(starts.tolist(), stops.tolist()):
            position = base + start
            state = _state(sum(int(x) << i for i, x in enumerate(bits[start:start + 32])))
            if self.previous_position is None:
                self.index = TagIndex()
                counter = self.index.index(state)
                self.first_counter = (counter - position) % TAG_PERIOD
                self.checked_start = position
            else:
                expected = tag_jump(self.previous_state, position - self.previous_position)
                if state == expected:
                    counter = (self.previous_counter + position - self.previous_position) % TAG_PERIOD
                    if position > self.previous_position + 1:
                        self.unexplained += 1
                else:
                    counter = self.index.index(state)
                    delta = (counter - self.previous_counter - position + self.previous_position) % TAG_PERIOD
                    if delta > TAG_PERIOD // 2:
                        delta -= TAG_PERIOD
                    self.events.append({"after_decoded_sample": self.previous_position + 32,
                                        "before_decoded_sample": position + 32,
                                        "source_jump_samples": int(delta),
                                        "kind": "gap" if delta > 0 else "repeat"})
            last = stop - 1
            last_state = _state(sum(int(x) << i for i, x in enumerate(bits[last:last + 32])))
            if tag_jump(state, last - start) != last_state:
                raise ValueError("inconsistent tag anchors")
            self.previous_position = base + last
            self.previous_counter = (counter + last - start) % TAG_PERIOD
            self.previous_state = last_state
            self.checked_stop = self.previous_position + 64

    def result(self) -> dict:
        if self.total < 256:
            raise ValueError("need at least 256 decoded tag samples")
        if self.first_counter is None or self.good_anchors / (self.total - 63) < .90:
            raise ValueError("tag recurrence acquisition failed (noise, filtering or timing)")
        return {"first_source_sample": int(self.first_counter), "decoded_samples": self.total,
                "checked_start": self.checked_start, "checked_stop": self.checked_stop,
                "edge_unchecked_samples": self.checked_start + self.total - self.checked_stop,
                "recurrence_errors": self.errors, "unexplained_error_regions": self.unexplained,
                "gaps": sum(e["source_jump_samples"] for e in self.events if e["source_jump_samples"] > 0),
                "repeats": -sum(e["source_jump_samples"] for e in self.events if e["source_jump_samples"] < 0),
                "events": self.events,
                "continuity_pass": self.errors == 0 and not self.events and self.checked_start == 0
                                   and self.checked_stop == self.total}


def decode_bits(bits: np.ndarray) -> dict:
    """Sequence verification after coherent sampling, also used in unit tests."""
    verifier = TagVerifier()
    for first in range(0, len(bits), 262144):
        verifier.feed(bits[first:first + 262144])
    return verifier.result()


def analyze_tag(capture, rate: float) -> dict:
    if rate < 50_000_000:
        raise ValueError("tag needs >=50 MS/s and the full 40 MHz RF band; a narrow capture cannot prove sample continuity")
    if len(capture) < 32768:
        raise ValueError("tag needs at least 32768 ADC samples for carrier/timing acquisition")
    block_size = 16384
    z = capture.read(0, min(len(capture), 131072))
    omega, carrier_phase = _carrier(z)
    q = DAC_RATE / rate
    phase = None
    verifier = TagVerifier()
    eye_costs, clock_points = [], []
    next_tick = None
    polarity = 1
    previous_axis = 0.0
    first_adc_sample, last_adc_sample = None, None
    for first in range(0, len(capture), block_size):
        last = min(first + block_size + 32, len(capture))
        if last - first < 32:
            break
        z = capture.read(first, last)
        z *= np.exp(-1j * (omega * (first + np.arange(len(z))) + carrier_phase))
        axis = .5 * np.angle(np.dot(z, z))
        axis += round((previous_axis - axis) / np.pi) * np.pi
        previous_axis = axis
        real = (z * np.exp(-1j * axis)).real
        # Local phase must follow the same global DAC time grid.
        predicted = None if phase is None else phase + q * block_size
        if len(real) < 1024 and predicted is not None:
            phase = predicted
            cost = _eye_cost(real, q, phase)
        else:
            q, phase, cost = _timing(real, q, predicted)
        samples, times = _sample(real, q, phase)
        ticks = np.rint(q * times + phase).astype(np.int64)
        select = times < min(block_size + 8, len(real) - 9)
        if next_tick is not None:
            select &= ticks >= next_tick
        samples, times, ticks = samples[select], times[select], ticks[select]
        if not len(samples):
            raise ValueError("tag timing did not produce samples")
        if next_tick is not None and ticks[0] != next_tick:
            raise ValueError("tag timing lost a sample between processing blocks")
        if first_adc_sample is None:
            first_adc_sample = first + float(times[0])
        last_adc_sample = first + float(times[-1])
        bits = (samples > 0).astype(np.uint8)
        if first == 0:
            err = _checks(bits).mean()
            inv_err = _checks(1 - bits).mean()
            polarity = 1 if err < inv_err else -1
        if polarity < 0:
            bits = 1 - bits
        verifier.feed(bits)
        next_tick = ticks[-1] + 1
        eye_costs.append(cost)
        clock_points.append((first + float(times[len(times) // 2]), float(ticks[len(ticks) // 2])))
    if max(eye_costs) > .65:
        raise ValueError("tag eye is closed or noisy (constant-modulus error >0.65)")
    result = verifier.result()
    adc_times, dac_ticks = np.array(clock_points).T
    slope, intercept = np.polyfit(adc_times, dac_ticks, 1)
    residual = dac_ticks - (slope * adc_times + intercept)
    if len(clock_points) < 3 or np.max(abs(residual)) > .35:
        raise ValueError("tag clock acquisition is ambiguous")
    result.update({"signal": "tag", "sample_rate_hz": rate,
                   "capture_samples": len(capture),
                   "adc_unchecked_start_samples": math.ceil(first_adc_sample),
                   "adc_unchecked_end_samples": math.ceil(len(capture) - 1 - last_adc_sample),
                   "clock_observation_seconds": float((adc_times[-1] - adc_times[0]) / rate),
                   "dac_sample_rate_hz": float(slope * rate),
                   "dac_rate_relative_to_40mhz_ppm": float((slope * rate / DAC_RATE - 1) * 1e6),
                   "carrier_offset_hz": float(omega * rate / (2 * np.pi)),
                   "clock_fit_max_error_samples": float(max(abs(residual))),
                   "eye_error_max": float(max(eye_costs)),
                   "rate_reference": "SDR sample-rate argument; inherits SDR clock calibration",
                   "scope": "decoded capture interior; requires qualified full-band receiver"})
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--format", choices=("i16", "f32"), required=True)
    parser.add_argument("--rate", type=float, required=True, help="calibrated complex sample rate in samples/s")
    parser.add_argument("--signal", choices=("tone", "two", "tag", "lines", "gap"), required=True)
    parser.add_argument("--dac-rate", type=float, default=DAC_RATE,
                        help="gap: the DAC rate of the run (40e6; 80e6 uses lead 128, tail 512)")
    parser.add_argument("--scale", type=float, help="full scale, e.g. 2048 for E200 int16 captures")
    args = parser.parse_args()
    try:
        if not math.isfinite(args.rate) or args.rate <= 0:
            raise ValueError("sample rate must be positive and finite")
        capture = Capture(args.capture, args.format, args.scale)
        if args.signal == "tag":
            result = analyze_tag(capture, args.rate)
        elif args.signal == "lines":
            result = lines(capture, args.rate)
        elif args.signal == "gap":
            lead, tail = (128, 512) if args.dac_rate == 80e6 else (64, 2048)
            result = gaps(capture, args.rate, args.dac_rate, 16380 - lead - tail, lead, tail)
        else:
            result = spectrum(capture, args.rate, args.signal)
            result["modulation"] = modulation(capture, args.rate, result["tones"][0]["offset_hz"])
        print(json.dumps(result, allow_nan=False))
        return 0 if args.signal != "tag" or result["continuity_pass"] else 1
    except (ValueError, OSError) as error:
        print(json.dumps({"error": str(error)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
