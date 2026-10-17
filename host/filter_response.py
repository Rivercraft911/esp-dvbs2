#!/usr/bin/env python3
"""Measure stationary bursts from `tx ... sweep ... 80`, entirely offline.

Reports the combined TX/path/RX response in dBFS. A matched narrow/wide ratio
cancels a stable receiver response; absolute TX flatness still needs calibration.
Only read captures taken inside the sweep payload, excluding the CW station ID.
"""
import argparse
import json
import math
import pathlib
import sys

import numpy as np

from capture import Capture


def db(power):
    return 10 * math.log10(max(float(power), 1e-30))


def point(capture, rate=61_440_000, lo_offset=0, size=32768):
    """Hann-windowed line powers; reject sweeps that change frequency within a burst.

    lo_offset is RX LO minus requested TX carrier, in Hz. Carrier error must be
    below 150 kHz; this includes board/receiver crystal error and DAC clock error.
    Integrate +/-4 bins, estimate local noise, and subtract that noise from lines.
    """
    if not math.isfinite(rate) or rate < 50e6 or not math.isfinite(lo_offset):
        raise ValueError('need a finite >=50 MS/s rate and finite LO offset')
    if len(capture) < 4 * size:
        raise ValueError('need at least four FFT blocks')
    window = np.hanning(size)
    norm = size * np.dot(window, window)
    freqs = np.fft.fftfreq(size, 1 / rate)
    total = np.zeros(size)
    offsets, peak_fs = [], 0.0
    for first in range(0, len(capture) - size + 1, size):
        z = capture.read(first, size)
        peak_fs = max(peak_fs, float(max(abs(z.real).max(), abs(z.imag).max())))
        psd = abs(np.fft.fft(z * window)) ** 2 / norm
        # Skip receiver DC, which can exceed a weak edge tone.
        search = (abs(freqs) > 200000) & (abs(freqs + lo_offset) < 20.2e6)
        peak = np.argmax(np.where(search, psd, -1))
        offsets.append(float(freqs[peak]))
        total += psd
    if peak_fs >= .9:
        raise ValueError('receiver peak >=90% of full scale')
    if max(offsets) - min(offsets) > 2 * rate / size:
        raise ValueError('tone changed during burst (sweep boundary, ID, or interference)')
    measured = float(np.median(offsets))
    mhz = round((measured + lo_offset) / 1e6)
    carrier = measured - mhz * 1e6
    if not 1 <= abs(mhz) <= 20 or abs(carrier + lo_offset) > 150000:
        raise ValueError('not an expected +/-1..20 MHz sweep tone')
    if abs(carrier) < 10 * rate / size:
        raise ValueError('receiver DC overlaps TX carrier; shift RX LO')
    total /= len(offsets)
    width = 4 * rate / size
    noise_region = (abs(freqs - measured) > 100000) & (abs(freqs - measured) < 200000)
    # Exponential-noise median/ln(2) estimates mean bin power, robust to sparse spurs.
    noise_bin = np.median(total[noise_region]) / math.log(2)

    def line(f):
        if abs(f) + width >= rate / 2:
            raise ValueError('line outside receiver Nyquist band')
        mask = abs(freqs - f) <= width
        noise = noise_bin * np.count_nonzero(mask)
        return max(float(total[mask].sum()) - noise, 1e-30), max(noise, 1e-30)

    power, noise = line(measured)
    image, image_noise = line(2 * carrier - measured)
    leak, leak_noise = line(carrier)
    snr = db(power / noise)
    if snr < 30:
        raise ValueError('tone SNR <30 dB in integration band')
    return dict(offset_mhz=mhz, tone_hz=measured, carrier_hz=carrier,
                power_dbfs=db(power), image_dbc=db(max(image, 3 * image_noise) / power),
                carrier_dbc=db(max(leak, 3 * leak_noise) / power),
                image_upper_limit=image < 3 * image_noise, carrier_upper_limit=leak < 3 * leak_noise,
                snr_db=snr, peak_fs=peak_fs, fft_bin_hz=rate / size,
                integration_half_width_hz=width, blocks=len(offsets))


def summarize(points):
    groups = {}
    for p in points:
        groups.setdefault(p['offset_mhz'], []).append(p['power_dbfs'])
    powers = {k: float(np.median(v)) for k, v in groups.items()}
    if -1 not in powers or 1 not in powers:
        raise ValueError('need both +/-1 MHz references')
    reference = (powers[-1] + powers[1]) / 2
    rows = [dict(offset_mhz=k, count=len(groups[k]), power_dbfs=powers[k],
                 relative_db=powers[k] - reference) for k in sorted(powers)]
    missing = [k for k in range(-20, 21) if k and len(groups.get(k, [])) < 3]
    inside = [r['relative_db'] for r in rows if abs(r['offset_mhz']) <= 18]
    return dict(scope='TX plus RF path plus RX; not calibrated TX bandwidth',
                reference_dbfs=reference, points=rows, missing_three_bursts=missing,
                flatness_36mhz_db=max(inside) - min(inside),
                complete_36mhz=not any(abs(k) <= 18 for k in missing),
                complete_40mhz=not missing)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('captures', nargs='+', type=pathlib.Path)
    parser.add_argument('--rate', type=float, default=61_440_000)
    parser.add_argument('--lo-offset', type=float, default=0, help='RX LO minus TX carrier, Hz')
    args = parser.parse_args()
    points, rejected = [], []
    for path in args.captures:
        try:
            p = point(Capture(path, 'i16', 2048), args.rate, args.lo_offset)
            points.append(dict(capture=str(path), **p))
        except (OSError, ValueError) as error:
            rejected.append(dict(capture=str(path), error=str(error)))
    try:
        summary = summarize(points)
    except ValueError as error:
        summary = dict(error=str(error))
    print(json.dumps(dict(bursts=points, rejected=rejected, summary=summary), allow_nan=False))
    return 0 if summary.get('complete_40mhz') else 1


if __name__ == '__main__':
    sys.exit(main())
