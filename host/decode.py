"""Run a separate LeanDVB process on cf32_le IQ and check its decoded BBFRAMEs.

Local receiver build and the small macOS/status patch: notes/decode.md. No receiver
code is imported here. GCS output contains the byte-aligned DATA FIELDs; TS output
comes from the receiver's packet deframer. Acquisition may discard leading frames.
An expected file must occur completely and contiguously in the recovered stream.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RECEIVER = ROOT / "notes/leansdr/src/apps/leandvb"
FEC_STATUS = re.compile(
    r"S2FEC modcod=(\d+) short=(\d+) pilots=(\d+) ldpc_flips=(\d+) "
    r"ldpc_checks_before=(\d+) ldpc_checks_after=(\d+) bch_errors=(-?\d+)")
MODCODS = {4: "QPSK 1/2", 6: "QPSK 2/3", 7: "QPSK 3/4",
           13: "8PSK 2/3", 15: "8PSK 5/6",
           18: "16APSK 2/3", 19: "16APSK 3/4", 20: "16APSK 4/5", 21: "16APSK 5/6",
           22: "16APSK 8/9", 23: "16APSK 9/10", 24: "32APSK 3/4", 25: "32APSK 4/5",
           26: "32APSK 5/6", 27: "32APSK 8/9", 28: "32APSK 9/10"}


def crc8(data):
    """EN 302 307-1 5.1.6: BBHEADER CRC, polynomial 0xd5, MSB first."""
    reg = 0
    for byte in data:
        for shift in range(7, -1, -1):
            feedback = (reg >> 7) ^ ((byte >> shift) & 1)
            reg = ((reg << 1) & 255) ^ (0xd5 if feedback else 0)
    return reg


def parse_bbframes(raw):
    """Parse upstream --fd-bbf records: BBHEADER + DFL bits, no padding."""
    frames, offset = [], 0
    while offset < len(raw):
        header = raw[offset:offset + 10]
        if len(header) != 10:
            raise ValueError(f"truncated BBHEADER at byte {offset}")
        if crc8(header[:9]) != header[9]:
            raise ValueError(f"BBHEADER CRC failed at byte {offset}")
        dfl = int.from_bytes(header[4:6], "big")
        if dfl % 8:
            raise ValueError("receiver interface supports byte-aligned DFL only")
        end = offset + 10 + dfl // 8
        if dfl > 58112 or end > len(raw):
            raise ValueError(f"invalid or truncated DATA FIELD at byte {offset}")
        stream_type = header[0] >> 6
        if stream_type not in (1, 3) or not header[0] & 32:
            raise ValueError("only single-stream GCS and TS captures are supported")
        if stream_type == 1 and (header[2:4] != b"\0\0" or header[7:9] != b"\0\0"):
            raise ValueError("GCS requires UPL=0 and SYNCD=0")
        frames.append({"stream_type": stream_type, "dfl_bits": dfl,
                       "rolloff_code": header[0] & 3,
                       "payload": raw[offset + 10:end]})
        offset = end
    return frames


def capture_info(path, sample_rate=None, symbol_rate=None, alpha=None):
    """Resolve raw cfile or a single cf32_le SigMF recording."""
    path = Path(path)
    metadata = {}
    if path.suffix in (".sigmf-data", ".sigmf-meta"):
        meta_path = path.with_suffix(".sigmf-meta")
        metadata = json.loads(meta_path.read_text())
        global_info = metadata["global"]
        if global_info["core:datatype"] != "cf32_le":
            raise ValueError("convert SigMF samples to cf32_le before decoding")
        if global_info.get("core:num_channels", 1) != 1 or "core:dataset" in global_info:
            raise ValueError("SigMF must be a single channel with a paired data filename")
        if sample_rate is None:
            sample_rate = global_info["core:sample_rate"]
        if symbol_rate is None:
            symbol_rate = global_info.get("dvbs2:symbol_rate")
        if alpha is None:
            alpha = global_info.get("dvbs2:rolloff")
        path = path.with_suffix(".sigmf-data")
        if global_info.get("core:offset", 0) or global_info.get("core:trailing_bytes", 0):
            raise ValueError("SigMF data must contain samples only")
        if any(c.get("core:sample_start", 0) for c in metadata.get("captures", [])):
            raise ValueError("split multi-segment SigMF captures before decoding")
        if any(c.get("core:header_bytes", 0) for c in metadata.get("captures", [])):
            raise ValueError("remove SigMF segment headers before decoding")
    for name, value in (("sample rate", sample_rate), ("symbol rate", symbol_rate)):
        if value is None or not math.isfinite(value) or value <= 0:
            raise ValueError(f"a positive {name} is required")
    if sample_rate < 2 * symbol_rate:
        raise ValueError("LeanDVB requires at least two samples per symbol")
    alpha = 0.35 if alpha is None else alpha
    if alpha not in (0.20, 0.35):
        raise ValueError("roll-off must be 0.20 or 0.35")
    if path.stat().st_size % 8:
        raise ValueError("cf32_le data length must be a multiple of 8 bytes")
    return path, float(sample_rate), float(symbol_rate), alpha


def summarize(status, frames, payload, expected=None):
    attempts = [tuple(map(int, m)) for m in FEC_STATUS.findall(status)]
    lock_events = [int(x) for x in re.findall(r"\bFRAMELOCK (\d+)\b", status)]
    modcods = sorted({a[0] for a in attempts if a[6] >= 0})
    report = {
        "receiver": "leandvb", "frame_lock": 1 in lock_events,
        "lock_at_end": bool(lock_events and lock_events[-1]),
        "modcods": modcods,
        "attempted_modcods": sorted({a[0] for a in attempts}),
        "modes": [MODCODS.get(m, f"MODCOD {m}") for m in modcods],
        "fec_frames": len(attempts) if attempts else None,
        "bb_frames": len(frames),
        "empty_bb_frames": sum(f["dfl_bits"] == 0 for f in frames),
        "ldpc_bit_flips": sum(a[3] for a in attempts) if attempts else None,
        "ldpc_checks_before": sum(a[4] for a in attempts) if attempts else None,
        "ldpc_checks_after": sum(a[5] for a in attempts) if attempts else None,
        "ldpc_residual_frames": sum(a[5] > 0 for a in attempts) if attempts else None,
        "bch_corrected_bits": sum(max(a[6], 0) for a in attempts) if attempts else None,
        "bch_failed_frames": sum(a[6] < 0 for a in attempts) if attempts else None,
        "stream": ("gcs" if frames[0]["stream_type"] == 1 else "ts") if frames else None,
        "payload_bytes": len(payload),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "status_complete": bool(attempts),
    }
    # fd-bbf contains valid headers only. Count receiver-side rejection messages too.
    report["bbheader_rejected_frames"] = status.count("Bad bbframe")
    if expected is not None:
        if not expected:
            raise ValueError("expected payload must not be empty")
        match = payload.find(expected)
        report.update({"expected_bytes": len(expected),
                       "expected_sha256": hashlib.sha256(expected).hexdigest(),
                       "payload_match": match >= 0,
                       "match_offset_bytes": match if match >= 0 else None,
                       "compared_bits": len(expected) * 8 if match >= 0 else 0,
                       "payload_bit_errors": 0 if match >= 0 else None})
    return report


def decode(capture, *, sample_rate=None, symbol_rate=None, alpha=None,
           receiver=None, expected=None, output=None, log=None, tune=0.0, timeout=120,
           rrc_rej=20):
    """Decode one file, returning a JSON-serializable report and recovered bytes.

    LDPC counts are bit flips and unsatisfied parity checks, not a measured BER.
    BCH failures drop frames. FRAMELOCK is PL synchronization, also valid for GCS;
    LeanDVB's TS-only LOCK signal is deliberately not used for GCS.
    """
    path, fs, rs, alpha = capture_info(capture, sample_rate, symbol_rate, alpha)
    executable = str(receiver or os.environ.get("LEANDVB", DEFAULT_RECEIVER))
    if not shutil.which(executable):
        raise FileNotFoundError(f"LeanDVB not found: {executable}; build per notes/decode.md")
    if not math.isfinite(tune) or timeout <= 0 or not math.isfinite(rrc_rej) or rrc_rej <= 0:
        raise ValueError("tune must be finite; timeout and RRC rejection must be positive")
    with tempfile.TemporaryFile() as bb, tempfile.TemporaryFile() as ts, \
            tempfile.TemporaryFile() as diagnostics, path.open("rb") as source:
        command = [executable, "--standard", "DVB-S2", "--f32", "-f", str(fs),
                   "--sr", str(rs), "--sampler", "rrc", "--rrc-rej", str(rrc_rej), "--fastdrift",
                   "--roll-off", str(alpha), "--ldpc-bf", "100",
                   "--fd-bbf", str(bb.fileno()), "--fd-info", "2", "-d"]
        if tune:
            command += ["--tune", str(tune)]
        result = subprocess.run(command, stdin=source, stdout=ts, stderr=diagnostics,
                                pass_fds=(bb.fileno(),), timeout=timeout, check=False)
        diagnostics.seek(0)
        status = diagnostics.read().decode("utf-8", errors="replace")
        if log:
            Path(log).write_text(status)
        if result.returncode:
            raise RuntimeError(f"LeanDVB exited {result.returncode}: {status[-2000:]}")
        bb.seek(0)
        frames = parse_bbframes(bb.read())
        if len({f["stream_type"] for f in frames}) > 1:
            raise ValueError("mixed GCS/TS captures require separate output streams")
        if frames and frames[0]["stream_type"] == 3:
            ts.seek(0)
            payload = ts.read()
            if len(payload) % 188 or any(payload[k] != 0x47 for k in range(0, len(payload), 188)):
                raise ValueError("receiver emitted an invalid TS packet stream")
        else:
            payload = b"".join(f["payload"] for f in frames)
    report = summarize(status, frames, payload, expected)
    report.update({"sample_rate": fs, "symbol_rate": rs, "rolloff": alpha,
                   "rrc_rej_db": rrc_rej, "command": command})
    if output:
        Path(output).write_bytes(payload)
    return report, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--sample-rate", type=float, help="Hz, required for raw cfile")
    parser.add_argument("--symbol-rate", type=float, help="symbols/s")
    parser.add_argument("--alpha", type=float, choices=(0.20, 0.35))
    parser.add_argument("--receiver", type=Path, help="local patched leandvb executable")
    parser.add_argument("--expected", type=Path, help="complete payload to find contiguously")
    parser.add_argument("--output", type=Path, required=True, help="all recovered GCS/TS bytes")
    parser.add_argument("--log", type=Path, help="save unabridged receiver diagnostics")
    parser.add_argument("--tune", type=float, default=0, help="receiver frequency bias in Hz")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--rrc-rej", type=float, default=20,
                        help="receiver RRC filter rejection in dB; APSK loopbacks use 40")
    args = parser.parse_args()
    try:
        report, _ = decode(args.capture, sample_rate=args.sample_rate,
                           symbol_rate=args.symbol_rate, alpha=args.alpha,
                           receiver=args.receiver,
                           expected=args.expected.read_bytes() if args.expected else None,
                           output=args.output, log=args.log, tune=args.tune, timeout=args.timeout,
                           rrc_rej=args.rrc_rej)
        print(json.dumps(report, sort_keys=True))
        return 0 if (report["frame_lock"] and report["bb_frames"] and
                     report["status_complete"] and report.get("payload_match", True)) else 1
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(2, f"decode: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
