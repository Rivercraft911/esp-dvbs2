"""Verify a saved synthetic VCM board dump with LeanDVB and the host BBFRAME model.

Offline only: no ports, SDR, or board commands. The JSONL supplies the pattern,
callsign, sample geometry, nominal capture length and CRC from the same dump.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zlib

import numpy as np

import decode
import dvbs2 as d
import morse


def expected_frames(pattern, count, hold, callsign):
    if not pattern or any(mc not in range(4, 18) for mc in pattern) or count < 1 or hold < 1:
        raise ValueError("need a MODCOD 4..17 pattern, positive frame count and dwell")
    header = ("ESP-DVBS2 ID=" + morse.callsign(callsign)).ljust(39).encode("ascii") + b"\n" if callsign else None
    seed, result = 2026, []
    for index in range(count):
        mc = pattern[(index // hold) % len(pattern)]
        rate = d.RATES[mc-1] if mc < 12 else d.PSK8_RATES[mc-12]
        size = (d.code(rate).kbch-80)//8
        payload = bytearray(size)
        for i in range(size):
            seed ^= (seed << 13) & 0xffffffff
            seed ^= seed >> 17
            seed ^= (seed << 5) & 0xffffffff
            payload[i] = seed & 255
        if header:
            payload[:40] = header
        bits = np.unpackbits(np.frombuffer(payload, np.uint8))
        bbf = np.packbits(d.bbframe(bits, rate, ccm=0)).tobytes()
        result.append(dict(index=index, modcod=mc, bbf=bbf, payload=bytes(payload)))
    return result


def verify(raw_bbf, status, expected):
    frames = decode.parse_bbframes(raw_bbf)
    payload = b"".join(frame["payload"] for frame in frames)
    report = decode.summarize(status, frames, payload)
    if not all(report[key] for key in ("frame_lock", "lock_at_end", "status_complete")):
        raise ValueError("missing receiver lock or FEC diagnostics")
    for key in ("ldpc_bit_flips", "ldpc_checks_after", "bch_corrected_bits",
                "bch_failed_frames", "bbheader_rejected_frames"):
        if report[key] != 0:
            raise ValueError(f"receiver reported {key}={report[key]}")
    attempts = [tuple(map(int, row)) for row in decode.FEC_STATUS.findall(status)]
    if len(attempts) != len(frames) or any(row[1:3] != (0, 1) for row in attempts):
        raise ValueError("need one normal, pilot-on PLS/FEC diagnostic per BBFRAME")
    if len(frames) < max(1, len(expected)-2):
        raise ValueError("more than two nominal frames missing at acquisition/EOF")
    # Keep complete BBHEADERs: CCM, DFL, roll-off and CRC must match too.
    actual, offset = [], 0
    for frame, fec in zip(frames, attempts):
        length = 10 + len(frame["payload"])
        actual.append(dict(modcod=fec[0], bbf=raw_bbf[offset:offset+length]))
        offset += length
    matches = [start for start in range(len(expected)-len(actual)+1)
               if all(got["modcod"] == want["modcod"] and got["bbf"] == want["bbf"]
                      for got, want in zip(actual, expected[start:]))]
    if len(matches) != 1:
        raise ValueError("PLS MODCODs and full BBFRAME bytes do not uniquely match a contiguous host-model interval")
    start = matches[0]
    manifest, per_mode = [], defaultdict(lambda: dict(frames=0, payload_bytes=0))
    for frame in expected[start:start+len(actual)]:
        mc, data = frame["modcod"], frame["payload"]
        per_mode[mc]["frames"] += 1
        per_mode[mc]["payload_bytes"] += len(data)
        manifest.append(dict(frame=frame["index"], modcod=mc, payload_bytes=len(data),
                             bbheader=frame["bbf"][:10].hex(),
                             payload_sha256=hashlib.sha256(data).hexdigest()))
    if set(per_mode) != {frame["modcod"] for frame in expected}:
        raise ValueError("not every expected MODCOD was recovered")
    report.update(exact_bbframes=True, contiguous_payload=True, payload_bit_errors=0,
                  compared_bits=8*len(payload), first_frame=start, last_frame=start+len(actual)-1,
                  nominal_frames=len(expected), omitted_leading_frames=start,
                  omitted_trailing_frames=len(expected)-start-len(actual), per_modcod=dict(per_mode))
    return report, manifest, payload


def evidence(capture, records):
    rows = [json.loads(line) for line in records.read_text().splitlines() if line.strip()]
    dumps = [row for row in rows if row.get("cmd") == "dvbdump" and "status" in row]
    binaries = [row for row in rows if row.get("cmd") == "dvbdump" and "binary_bytes" in row]
    if len(dumps) != 1 or len(binaries) != 1:
        raise ValueError("records must describe exactly one dump")
    dump, binary = dumps[0], binaries[0]
    pattern = [row for row in rows if row.get("cmd") == "vcm" and row.get("enabled") and row.get("pattern")]
    if len(pattern) != 1 or not dump.get("vcm") or dump["status"] != "ESP_OK":
        raise ValueError("need one successful patterned VCM dump")
    if dump.get("input") != "synthetic" or dump.get("payload_source") != "xorshift32":
        raise ValueError("only the synthetic continuous seed-2026 payload is supported")
    if dump["underruns"] or dump["overlap_mismatches"] or dump["encoder_failed"]:
        raise ValueError("dump reports underruns, overlap errors or encoder failure")
    if dump["rolloff"] != .35 or binary["format"] != "int16 I/Q LE":
        raise ValueError("need roll-off .35 and signed int16 LE I/Q")
    config = pattern[0]
    expected = expected_frames(config["pattern"], dump["frames"], config["hold_frames"], dump["callsign"])
    symbols = sum(d.plframe_length(False, True, "qpsk" if f["modcod"] < 12 else "8psk") for f in expected)
    samples = (symbols*dump["sps_num"]+dump["sps_den"]-1)//dump["sps_den"]
    size = (samples+dump["decimation"]-1)//dump["decimation"]*4
    raw = capture.read_bytes()
    crc = zlib.crc32(raw)
    sample_rate = dump["symbol_rate"]*dump["sps_num"]/dump["sps_den"]/dump["decimation"]
    if samples != dump["samples"] or size != len(raw) or size != binary["binary_bytes"] or size % 4:
        raise ValueError("capture length differs from the nominal pattern/sample geometry")
    if sample_rate != binary["sample_rate"] or crc != dump["crc32"] or crc != binary.get("host_crc32", crc):
        raise ValueError("capture sample rate or CRC differs from the board records")
    return raw, expected, dict(sample_rate=sample_rate, symbol_rate=dump["symbol_rate"],
                               callsign=dump["callsign"], pattern=config["pattern"],
                               hold_frames=config["hold_frames"], capture_crc32=crc,
                               capture_sha256=hashlib.sha256(raw).hexdigest(),
                               frames_played_counter=dump["frames_played"])


def run(capture, records, receiver, output):
    executable = shutil.which(str(receiver))
    if not executable:
        raise FileNotFoundError(f"LeanDVB not found: {receiver}")
    receiver = Path(executable).resolve()
    raw, expected, metadata = evidence(capture, records)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp)/"capture.cf32"
        iq = np.frombuffer(raw, "<i2").reshape(-1, 2).astype(np.float32)/512
        np.r_[iq[:, 0]+1j*iq[:, 1], np.zeros(4096, dtype="<c8")].astype("<c8").tofile(source)
        with source.open("rb") as samples, (output/"frames.bbf").open("wb") as bb, \
                (output/"leandvb.log").open("w") as log, tempfile.TemporaryFile() as ts:
            command = decode.receiver_command(receiver, metadata["sample_rate"], metadata["symbol_rate"], .35, bb.fileno())
            subprocess.run(command, stdin=samples, stdout=ts, stderr=log,
                           pass_fds=(bb.fileno(),), timeout=120, check=True)
    report, manifest, payload = verify((output/"frames.bbf").read_bytes(), (output/"leandvb.log").read_text(), expected)
    report.update(metadata, receiver_flush_samples=4096, receiver_executable=str(receiver),
                  receiver_sha256=hashlib.sha256(receiver.read_bytes()).hexdigest())
    (output/"payload.bin").write_bytes(payload)
    (output/"frames.jsonl").write_text("".join(json.dumps(row, sort_keys=True)+"\n" for row in manifest))
    (output/"report.json").write_text(json.dumps(report, indent=2, sort_keys=True)+"\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--records", type=Path, required=True)
    parser.add_argument("--receiver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run(args.capture, args.records, args.receiver, args.output)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(json.dumps(dict(error=str(exc))))
        return 1
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
