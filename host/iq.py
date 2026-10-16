"""DVB-S2 phase bytes or complex symbols to complex float32 baseband. No hardware access.

Phase p means exp(j*pi*p/4), as in dvbs2.py. RRC is root raised cosine, whose
frequency response is the positive square root of the raised cosine in ETSI
EN 302 307-1 V1.4.1, 5.6 and Annex A (pp. 35, 37-38):
https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf

The pulse has unit energy in symbol-time units. For independent unit-energy
symbols the interior waveform therefore has approximately unit mean power.
The default pulse is truncated to 12 symbols; the complete start/end tails
are retained. Files are little-endian interleaved I,Q float32 (cf32_le).
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path

import numpy as np
from scipy.signal import upfirdn


def rrc(t, alpha):
    """Dimensionless RRC pulse h(t/T), including its three removable poles.

    h(u) = [sin(pi*(1-alpha)*u) + 4*alpha*u*cos(pi*(1+alpha)*u)] /
           [pi*u*(1-(4*alpha*u)**2)]. Its integral squared is one.
    """
    if not 0 < alpha <= 1:
        raise ValueError("alpha must be in (0, 1]")
    t = np.asarray(t, dtype=np.float64)
    zero = np.isclose(t, 0, atol=1e-12, rtol=0)
    pole = np.isclose(np.abs(t), 1 / (4 * alpha), atol=1e-12, rtol=0)
    regular = ~(zero | pole)
    h = np.empty_like(t)
    u = t[regular]
    h[regular] = (np.sin(np.pi * (1 - alpha) * u) +
                  4 * alpha * u * np.cos(np.pi * (1 + alpha) * u)) / (
                      np.pi * u * (1 - (4 * alpha * u) ** 2))
    h[zero] = 1 + alpha * (4 / np.pi - 1)
    h[pole] = alpha / np.sqrt(2) * (
        (1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) +
        (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
    return h


def _pulse(t, alpha, span, hold):
    # hold = Rs/Fdac in symbol periods. The integral is the baseband-equivalent
    # zero-order hold: H(f) = exp(-j*pi*f/Fdac)*sinc(f/Fdac). DAC images and an
    # analogue receiver's rejection of them are outside this model.
    if not hold:
        return np.where(np.abs(t) <= span / 2, rrc(t, alpha), 0)
    nodes, weights = np.polynomial.legendre.leggauss(8)
    out = np.zeros_like(t)
    for node, weight in zip(nodes, weights):
        u = t - hold * (node + 1) / 2
        out += weight / 2 * np.where(np.abs(u) <= span / 2, rrc(u, alpha), 0)
    return out


def shape(phases, symbol_rate, sample_rate, alpha=0.35, span=12, *, dac_rate=None,
          snr_db=None, frequency_offset=0.0, sample_rate_offset_ppm=0.0, seed=0):
    """Shape QPSK/8PSK phase bytes. See shape_symbols for the channel conventions."""
    phases = np.asarray(phases)
    if phases.ndim != 1 or not len(phases):
        raise ValueError("phases must be a nonempty one-dimensional array")
    if not np.issubdtype(phases.dtype, np.integer) or np.any((phases < 0) | (phases > 7)):
        raise ValueError("phase bytes must be integers from 0 through 7")
    return shape_symbols(np.exp(1j * np.pi / 4 * phases), symbol_rate, sample_rate, alpha,
                         span, dac_rate=dac_rate, snr_db=snr_db,
                         frequency_offset=frequency_offset,
                         sample_rate_offset_ppm=sample_rate_offset_ppm, seed=seed)


def shape_symbols(symbols, symbol_rate, sample_rate, alpha=0.35, span=12, *, dac_rate=None,
                  snr_db=None, frequency_offset=0.0, sample_rate_offset_ppm=0.0, seed=0):
    """Shape complex symbols, then apply capture-clock error, CFO and complex AWGN.

    Rates and frequency_offset are in Hz (symbol_rate in symbols/s). Positive
    sample_rate_offset_ppm means the capture clock runs faster than sample_rate:
    Fs_actual = sample_rate*(1+ppm*1e-6). The decoder is told the nominal rate.
    CFO is exp(j*2*pi*frequency_offset*n/Fs_actual), positive counter-clockwise.
    snr_db is *sample* signal/noise power, before matched filtering. With an
    ideal matched filter Es/N0 = snr_db + 10*log10(Fs_actual/symbol_rate).
    No amplitude normalization or clipping is applied. dac_rate adds the
    baseband response of an ideal DAC hold; it does not model DAC quantization.
    """
    symbols = np.asarray(symbols, np.complex128)
    if symbols.ndim != 1 or not len(symbols) or not np.all(np.isfinite(symbols)):
        raise ValueError("symbols must be a nonempty finite one-dimensional array")
    values = (symbol_rate, sample_rate, alpha, frequency_offset, sample_rate_offset_ppm)
    if not all(np.isfinite(v) for v in values) or symbol_rate <= 0 or sample_rate <= 0:
        raise ValueError("rates must be positive and all settings finite")
    if not 0 < alpha <= 1 or not isinstance(span, int) or span < 2 or span % 2:
        raise ValueError("alpha must be in (0, 1] and span an even integer >= 2")
    actual_rate = sample_rate * (1 + sample_rate_offset_ppm * 1e-6)
    if actual_rate < symbol_rate * (1 + alpha):
        raise ValueError("capture sample rate must cover Rs*(1+alpha)")
    if dac_rate is not None and (not np.isfinite(dac_rate) or
                                 dac_rate < symbol_rate * (1 + alpha)):
        raise ValueError("DAC rate must cover Rs*(1+alpha)")
    if snr_db is not None and not np.isfinite(snr_db):
        raise ValueError("snr_db must be finite, or None for no noise")
    sps = actual_rate / symbol_rate
    hold = 0 if dac_rate is None else symbol_rate / dac_rate
    count = int(np.floor((len(symbols) - 1 + span + hold) * sps)) + 1
    ratio = Fraction(float(sps)).limit_denominator(100000)
    if ratio.numerator <= 100000 and abs(float(ratio) - sps) <= 1e-12 * sps:
        # Sampling the pulse on the common rational clock avoids interpolation.
        up, down = ratio.numerator, ratio.denominator
        t = np.arange(int(np.ceil((span + hold) * up)) + 1) / up - span / 2
        out = upfirdn(_pulse(t, alpha, span, hold), symbols, up=up, down=down)[:count]
    else:
        # An arbitrary clock ratio still has an exact time-domain definition:
        # x[m] = sum_k a[k]*h(m/sps - k - span/2). Work in bounded chunks.
        out = np.empty(count, np.complex128)
        for start in range(0, count, 65536):
            t = np.arange(start, min(start + 65536, count)) / sps - span / 2
            base = np.floor(t).astype(np.int64)
            chunk = np.zeros(len(t), np.complex128)
            for offset in range(-span // 2, span // 2 + int(np.ceil(hold)) + 2):
                k = base + offset
                valid = (k >= 0) & (k < len(symbols))
                chunk[valid] += symbols[k[valid]] * _pulse(t[valid] - k[valid],
                                                          alpha, span, hold)
            out[start:start + len(t)] = chunk
    if frequency_offset:
        out *= np.exp(2j * np.pi * frequency_offset * np.arange(len(out)) / actual_rate)
    if snr_db is not None:
        # Do not let the finite filter tails set the requested interior SNR.
        margin = int(np.ceil((span / 2 + hold) * sps))
        interior = out[margin:-margin] if len(out) > 2 * margin else out
        noise_power = np.mean(np.abs(interior) ** 2) * 10 ** (-snr_db / 10)
        rng = np.random.default_rng(seed)
        out += np.sqrt(noise_power / 2) * (rng.standard_normal(len(out)) +
                                           1j * rng.standard_normal(len(out)))
    return np.asarray(out, dtype=np.complex64)


def write_capture(path, samples, sample_rate, symbol_rate, alpha=0.35, **settings):
    """Write raw cf32_le, or .sigmf-data plus its paired .sigmf-meta file.

    SigMF core fields follow https://sigmf.org/. The optional local dvbs2
    namespace v0.1.0 defines global symbol_rate (symbols/s) and rolloff
    (dimensionless). Other simulation settings are core:description JSON text.
    sample_rate is nominal, even when the simulated capture clock has an error.
    """
    path = Path(path)
    np.asarray(samples, dtype="<c8").tofile(path)
    if path.suffix == ".sigmf-data":
        metadata = {
            "global": {"core:datatype": "cf32_le", "core:sample_rate": sample_rate,
                       "core:version": "1.2.6", "core:recorder": "esp-dvbs2 host/iq.py",
                       "core:extensions": [{"name": "dvbs2", "version": "0.1.0",
                                            "optional": True}],
                       "dvbs2:symbol_rate": symbol_rate, "dvbs2:rolloff": alpha,
                       "core:description": json.dumps({"symbol_rate": symbol_rate,
                                                        "alpha": alpha, **settings},
                                                       sort_keys=True)},
            "captures": [{"core:sample_start": 0}], "annotations": []}
        path.with_suffix(".sigmf-meta").write_text(json.dumps(metadata, indent=2) + "\n")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("phases", type=Path, help="uint8 phase bytes, each 0..7")
    parser.add_argument("--complex-symbols", action="store_true",
                        help="read little-endian complex float32 symbols instead of phase bytes")
    parser.add_argument("output", type=Path, help="raw .cfile or .sigmf-data")
    parser.add_argument("--symbol-rate", type=float, required=True, help="symbols/s")
    parser.add_argument("--sample-rate", type=float, required=True, help="nominal samples/s")
    parser.add_argument("--alpha", type=float, choices=(0.20, 0.35), default=0.35)
    parser.add_argument("--dac-rate", type=float, help="optional DAC hold rate, samples/s")
    parser.add_argument("--snr-db", type=float, help="sample SNR in dB, absent means no noise")
    parser.add_argument("--frequency-offset", type=float, default=0, help="carrier offset, Hz")
    parser.add_argument("--sample-rate-offset-ppm", type=float, default=0, help="capture clock ppm")
    parser.add_argument("--seed", type=int, default=0, help="noise RNG seed")
    args = parser.parse_args()
    options = {k: v for k, v in vars(args).items()
               if k not in ("phases", "output", "complex_symbols")}
    try:
        function, dtype = (shape_symbols, "<c8") if args.complex_symbols else (shape, np.uint8)
        samples = function(np.fromfile(args.phases, dtype), **options)
        write_capture(args.output, samples, **options)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps({"path": str(args.output), "samples": len(samples),
                      "sample_rate": args.sample_rate, "symbol_rate": args.symbol_rate}))


if __name__ == "__main__":
    main()
