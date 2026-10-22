"""Experimental RX bank from the checked-in 40 MS/s, alpha=.35 TX taps.

Design is fixed before packet tests: truncate the matched pulse to 32 samples,
use one common Q7 scale, and retain a Q15 timing derivative. The channel model
includes the DAC zero-order hold and both existing HB11 filters. It excludes
RF analog response, clipping and the TX's per-three-symbol table rounding.
Those exclusions prevent this report from qualifying an RF receiver.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
TX = ROOT / "firmware/main/shaper_taps.h"
BANK = ROOT / "firmware/rx/dsp/rx_filter_bank.h"
HB = np.array([274, 0, -1812, 0, 9730, 16384,
               9730, 0, -1812, 0, 274], dtype=float) / 32768
PHASES, TAPS, GRID = 64, 32, 8193
# Engineering admissions, not thresholds selected from recovered payloads.
LIMITS = {"snr_loss_db": .25, "isi_db": -30., "gain_span_db": .15,
          "quantization_snr_loss_db": .05}


def tx_taps():
    text = TX.read_text()
    body = re.search(r"shaper_taps_qpsk_n40_a35\[360\] = \{(.*?)\};", text, re.S)
    taps = np.array([int(x) for x in re.findall(r"-?\d+", body[1])], float)
    if len(taps) != 360 or not np.array_equal(taps, taps[::-1]):
        raise ValueError("unexpected TX pulse")
    return taps


def spectrum(f):
    """Centered TX + hold + HB response, normalized at DC; f in Hz."""
    taps = tx_taps()
    answer = np.einsum("fk,k->f", np.cos(2*np.pi*f[:, None]*(np.arange(360)-179.5)/40e6), taps)
    answer *= np.sinc(f/40e6)  # The centered DAC hold, not a raw dvbdump.
    for rate in (16e6, 8e6):
        answer *= np.einsum("fk,k->f", np.cos(2*np.pi*f[:, None]*(np.arange(11)-5)/rate), HB)
    return answer / (taps.sum() * HB.sum()**2)


def sampled_pulse(t, f, response, derivative=False):
    """Inverse Fourier integral in units of 4 MS/s input samples."""
    omega = 2*np.pi*f/4e6
    shape = np.asarray(t).shape
    flat = np.asarray(t).ravel()
    result = np.empty(len(flat))
    for first in range(0, len(flat), 128):
        a = flat[first:first+128, None] * omega
        v = -np.sin(a)*omega if derivative else np.cos(a)
        result[first:first+128] = 2*np.trapezoid(v*response, f, axis=1)/4e6
    return result.reshape(shape)


def design():
    f = np.linspace(0, 2e6, GRID)
    response = spectrum(f)
    t = np.arange(TAPS)[None, :] - 15.5 - np.arange(PHASES)[:, None]/PHASES
    h = sampled_pulse(t, f, response)
    scale = np.linalg.norm(h[0])  # ONE normalization for every phase.
    h /= scale
    # d h(k-center-mu)/d mu = -h'(k-center-mu).
    derivative = -sampled_pulse(t, f, response, True)/scale
    pulse = np.rint(128*h).astype(np.int64)
    deriv = np.rint(32768*derivative).astype(np.int64)
    if np.max(np.abs(pulse)) > 127 or np.max(np.abs(deriv)) > 32767:
        raise ValueError("fixed coefficient scales overflow; do not silently rescale")
    return pulse.astype(np.int8), deriv.astype(np.int16), h, f, response, scale


def report(bank=None):
    p, d, h, f, response, scale = design() if bank is None else bank
    # Independent continuous-time channel samples at all symbol offsets.
    lags = np.arange(-16, 17)
    t = np.arange(TAPS)[None, None, :] - 15.5
    t = t - np.arange(PHASES)[:, None, None]/PHASES - 4*lags[None, :, None]
    channel = sampled_pulse(t, f, response)/scale
    q = p.astype(float)/128
    symbols = np.einsum("pk,pmk->pm", q, channel)
    center = symbols[:, 16]
    isi = (np.sum(symbols**2, axis=1)-center**2)/center**2
    ideal_energy = 2*np.trapezoid(response**2, f)/4e6/scale**2
    snr = center**2/np.sum(q*q, axis=1)/ideal_energy
    float_center = np.einsum("pk,pk->p", h, channel[:, 16])
    float_snr = float_center**2/np.sum(h*h, axis=1)/ideal_energy
    metrics = {"snr_loss_db": float(np.max(-10*np.log10(snr))),
               "isi_db": float(np.max(10*np.log10(isi))),
               "gain_span_db": float(20*np.log10(center.max()/center.min())),
               "quantization_snr_loss_db": float(np.max(10*np.log10(float_snr/snr)))}
    checks = {name: metrics[name] <= limit for name, limit in LIMITS.items()}
    # A spectrum inventory, not an RF/blocker admission. No channel gain fit.
    freq = np.array([0., .5e6, .675e6, .725e6, 1e6, 1.5e6, 2e6])
    rx = np.einsum("fk,pk->fp", np.exp(-2j*np.pi*freq[:, None]*np.arange(TAPS)/4e6), q)
    power = np.abs(rx)/np.abs(rx[0])
    return {"experimental": True, "rf_qualified": False,
            "tx_sha256": hashlib.sha256(TX.read_bytes()).hexdigest(),
            "tap_count": TAPS, "phases": PHASES, "pulse_q": 7, "derivative_q": 15,
            "limits": LIMITS, "metrics": metrics, "checks": checks,
            "model_admitted": all(checks.values()),
            "pulse_l1_max": int(np.abs(p.astype(np.int32)).sum(axis=1).max()),
            "derivative_l1_max": int(np.abs(d.astype(np.int32)).sum(axis=1).max()),
            "rx_response_db_max": dict(zip(map(str, freq),
                map(float, 20*np.log10(np.maximum(power.max(axis=1), 1e-30))))),
            "limits_of_model": ["No RF analog response or blocker test",
                "No TX table rounding, ADC saturation or SC8 clipping model",
                "White noise at 4 MS/s; HB noise coloration not qualified",
                "No native timing, synchronization or payload recovery claim"]}


def header(bank=None):
    p, d, *_ = design() if bank is None else bank
    lines = ["/* Generated by host/rx_filter.py --write. Experimental matched bank. */",
             "/* Pulse Q7; derivative Q15 per raw sample. Common scales, no phase gain LUT. */"]
    for name, values, kind in (("pulse", p, "int8_t"), ("derivative", d, "int16_t")):
        lines.append(f"_Alignas(16) RX_FILTER_DRAM const {kind} rx_filter_{name}[64][32] = {{")
        for row in values:
            lines.append("    {" + ",".join(map(str, row.tolist())) + "},")
        lines.append("};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="regenerate the small C coefficient bank")
    args = parser.parse_args()
    bank = design()
    if args.write:
        BANK.write_text(header(bank))
    print(json.dumps(report(bank), indent=2, sort_keys=True))
