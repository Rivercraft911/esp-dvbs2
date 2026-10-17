#!/usr/bin/env python3
"""Make a moving H.264 test card, MPEG-TS and DVB-S2 IQ using host/dvbs2.py.

No hardware. Large generated files belong in notes/. Loop joins reset video timestamps.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
import dvbs2
import iq


def ffmpeg_path():
    path = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if path:
        return path
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        raise FileNotFoundError("install ffmpeg, or pip install imageio-ffmpeg, or set FFMPEG")


def make_fixture(directory, seconds=2, rs=5_000_000, modcod=13):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.iterdir()):
        raise FileExistsError(f"fixture directory must be empty: {directory}")
    rate, modulation = ("2/3", "8psk") if modcod == 13 else ("3/4", "qpsk")
    fs = rs * 2
    dfl = dvbs2.code(rate).kbch - 80
    length = dvbs2.plframe_length(False, True, modulation)
    capacity = dfl / length * rs
    ts = directory / "video.ts"
    command = [ffmpeg_path(), "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
               "testsrc2=size=960x480:rate=15", "-t", str(seconds), "-c:v", "libx264",
               "-preset", "ultrafast", "-tune", "zerolatency", "-g", "5", "-bf", "0",
               "-b:v", "800k", "-pix_fmt", "yuv420p", "-muxrate", str(int(capacity)),
               "-mpegts_flags", "+resend_headers", "-f", "mpegts", str(ts)]
    subprocess.run(command, check=True)
    start = time.perf_counter()
    packets = np.frombuffer(ts.read_bytes(), np.uint8).reshape(-1, 188)
    bbs = dvbs2.ts_bbframes(packets, rate)
    phases = []
    for n, bb in enumerate(bbs):
        # These existing, tested byte/group forms equal fecframe bit for bit.
        fec = dvbs2.ldpc_encode_groups(dvbs2.bch_encode_bytes(dvbs2.bb_scramble(bb), rate), rate)
        phases.append(dvbs2.plframe_phases(fec, rate, modulation=modulation))
        if n % 100 == 0:
            print(f"encoded {n}/{len(bbs)} frames", flush=True)
    symbols = np.concatenate(phases)
    # Circular pulse shaping: retain one complete period with both neighboring tails.
    padded = np.concatenate([symbols[-6:], symbols, symbols[:6]])
    z = iq.shape(padded, rs, fs)[24:24+len(symbols)*2].astype("<c8")
    target = directory / "video.cf32"
    z.tofile(target)
    meta = dict(format="cf32_le", sample_rate=fs, symbol_rate=rs, modcod=modcod,
                modulation=modulation, code_rate=rate, rolloff=.35, pilots=True,
                circular_pulse=True, frames=len(bbs), signal_seconds=len(z)/fs, payload_capacity_bps=capacity,
                iq_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                ts_sha256=hashlib.sha256(ts.read_bytes()).hexdigest(), command=command,
                encode_seconds=time.perf_counter()-start,
                description="synthetic moving 2:1 test card, not RF or a photographed panorama")
    (directory / "fixture.json").write_text(json.dumps(meta, indent=2) + "\n")
    return target, meta


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--seconds", type=float, default=2)
    p.add_argument("--symbol-rate", type=int, default=5_000_000)
    p.add_argument("--modcod", type=int, choices=(7, 13), default=13)
    a = p.parse_args()
    if not 0 < a.seconds <= 30 or a.symbol_rate <= 0:
        p.error("positive symbol rate and 0 < seconds <= 30 required")
    _, meta = make_fixture(a.directory, a.seconds, a.symbol_rate, a.modcod)
    print(json.dumps(meta))


if __name__ == "__main__":
    main()
