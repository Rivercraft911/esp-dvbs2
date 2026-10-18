#!/usr/bin/env python3
"""RF-free video TS -> repository DVB-S2 model -> IQ -> receive.py -> TS check.

Needs numpy/scipy and an independent LeanDVB executable; never imports a device
driver. The generated MPEG-2 test video and IQ files stay in the output directory.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
from scipy.signal import upfirdn

from check_ts import check
from make_ts import make_ts

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2 as d


def rrc(sps=2, alpha=.35, span=12):
    t = np.arange(-span*sps, span*sps+1, dtype=float) / sps
    taps = np.empty_like(t)
    for k, x in enumerate(t):
        if abs(x) < 1e-9:
            taps[k] = 1 + alpha * (4 / np.pi - 1)
        elif abs(abs(4*alpha*x) - 1) < 1e-9:
            taps[k] = alpha / np.sqrt(2) * ((1 + 2/np.pi) * np.sin(np.pi/(4*alpha)) +
                                          (1 - 2/np.pi) * np.cos(np.pi/(4*alpha)))
        else:
            taps[k] = (np.sin(np.pi*x*(1-alpha)) + 4*alpha*x*np.cos(np.pi*x*(1+alpha))) / (
                np.pi*x*(1-(4*alpha*x)**2))
    return taps


def encode(ts, path):
    """Normal QPSK 3/4, pilots, .35 RRC; 20 MS/s complex float32 IQ.

    Byte mode adaptation retains packet CRC/SYNCD across BBFRAME boundaries.
    Dummy frames model a 9 Mb/s producer feeding a 14.520762 Mb/s link.
    """
    bbframes = d.ts_bbframes(np.frombuffer(ts, np.uint8), "3/4")
    phases, total, dummy_count = [], 0, 0
    frame_symbols = d.plframe_length(False, True)
    payload_bytes = (d.code("3/4").kbch - 80) // 8
    dummy = d.dummy_phases()
    for n, bb in enumerate(bbframes):
        scrambled = d.bb_scramble(bb)
        fec = d.ldpc_encode_groups(d.bch_encode_bytes(scrambled, "3/4"), "3/4")
        phases.append(d.plframe_phases(fec, "3/4", pilots=True))
        total += frame_symbols
        # Only whole frames are sent; residual credit carries to the next frame.
        target = (n+1) * payload_bytes * 8 * 10e6 / 9e6
        while total + len(dummy) <= target:
            phases.append(dummy)
            total += len(dummy)
            dummy_count += 1
    symbols = d.symbols_from_phases(np.concatenate(phases))
    samples = (.5 * upfirdn(rrc(), symbols, up=2)).astype("<c8")
    with path.open("xb") as f:
        samples.tofile(f)
        # Receiver pipeline lookahead at EOF, outside the transmitted waveform.
        np.zeros(8192, dtype="<c8").tofile(f)
    return {"data_frames": len(bbframes), "dummy_frames": dummy_count,
            "sample_rate": 20e6, "symbol_rate": 10e6, "modcod": 7,
            "rolloff": .35, "pilots": True, "samples": len(samples),
            "iq_seconds": len(samples) / 20e6, "unencoded_tail_bytes": len(ts) - len(bbframes)*payload_bytes}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path, help="new output directory")
    p.add_argument("--receiver", default=os.environ.get("RX", "leandvb"))
    p.add_argument("--ts", type=Path, help="use an existing complete TS file instead of test video")
    p.add_argument("--frames", type=int, default=25, help="number of generated 25 fps pictures")
    a = p.parse_args()
    if a.frames < 5:
        p.error("use at least 5 video pictures for acquisition and repeated PSI")
    a.output.mkdir(parents=True, exist_ok=False)
    ts = a.ts.read_bytes() if a.ts else make_ts(a.frames)
    source_report = check(ts)
    (a.output / "source.ts").write_bytes(ts)
    report = encode(ts, a.output / "source.cf32")
    command = [sys.executable, str(Path(__file__).with_name("receive.py")),
               "--iq", str(a.output / "source.cf32"), "--receiver", a.receiver,
               "--output", str(a.output / "received.ts")]
    started = time.monotonic()
    with (a.output / "receiver.log").open("w") as log:
        subprocess.run(command, stderr=log, check=True, timeout=120)
    elapsed = time.monotonic() - started
    received = (a.output / "received.ts").read_bytes()
    recovered_report = check(received, ts)
    # Prevent a tiny coincidental acquisition fragment from passing.
    if len(received) < .90 * len(ts):
        raise ValueError("receiver recovered less than 90% of the offered TS")
    report.update(source=source_report, received=recovered_report,
                  received_fraction=len(received)/len(ts), decode_wall_seconds=elapsed,
                  command=command, scope="ideal model IQ; no hardware or RF; no live-throughput guarantee")
    (a.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
