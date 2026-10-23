"""Run a separate LeanDVB process on cf32_le IQ and check its decoded BBFRAMEs.

Build a separate receiver as described in examples/self-check/README.md. No receiver
code is imported here. GCS output contains the byte-aligned DATA FIELDs; TS output
comes from the receiver's packet deframer. Acquisition may discard leading frames.
An expected file must occur completely and contiguously in the recovered stream.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RECEIVER = "leandvb"
FEC_STATUS = re.compile(
    r"S2FEC modcod=(\d+) short=(\d+) pilots=(\d+) ldpc_flips=(\d+) "
    r"ldpc_checks_before=(\d+) ldpc_checks_after=(\d+) bch_errors=(-?\d+)")
SOFT_STATUS = re.compile(
    r"^S2SOFT modcod=(\d+) short=([01]) pilots=([01]) bch_errors=(-?\d+) "
    r"ldpc_parity=unavailable$", re.MULTILINE)
HELPER_STATUS = re.compile(
    r"^S2HELPER sent=(\d+) received=(\d+) pending=(\d+) exit=(\d+)$", re.MULTILINE)
HELPER_REQUIREMENT = (
    "Use a compatible external receiver/helper pair for one known normal-frame MODCOD. "
    "The stock receiver builder does not provide this mode.")
MODCODS = {4: "QPSK 1/2", 6: "QPSK 2/3", 7: "QPSK 3/4", 11: "QPSK 9/10",
           13: "8PSK 2/3", 15: "8PSK 5/6", 17: "8PSK 9/10",
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
        frames.append({"header": header, "stream_type": stream_type, "dfl_bits": dfl,
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


def summarize(status, frames, payload, expected=None, *, ldpc_helper=None):
    hard = [tuple(map(int, m)) for m in FEC_STATUS.findall(status)]
    soft = [tuple(map(int, m)) for m in SOFT_STATUS.findall(status)]
    external = bool(ldpc_helper) or bool(soft)
    attempts = ([(mc, short, pilots, None, None, None, bch)
                 for mc, short, pilots, bch in soft] if external else hard)
    lock_events = [int(x) for x in re.findall(r"\bFRAMELOCK (\d+)\b", status)]
    modcods = sorted({a[0] for a in attempts if a[6] >= 0})
    report = {
        "receiver": "leandvb", "frame_lock": 1 in lock_events,
        "lock_at_end": bool(lock_events and lock_events[-1]),
        "modcods": modcods,
        "frames_per_modcod": dict(sorted(Counter(a[0] for a in attempts if a[6] >= 0).items())),
        "frame_modcods": [a[0] for a in attempts if a[6] >= 0],
        "attempted_modcods": sorted({a[0] for a in attempts}),
        "modes": [MODCODS.get(m, f"MODCOD {m}") for m in modcods],
        "fec_frames": len(attempts) if attempts else None,
        "bb_frames": len(frames),
        "empty_bb_frames": sum(f["dfl_bits"] == 0 for f in frames),
        "payload_frames": sum(f["dfl_bits"] > 0 for f in frames),
        "ldpc_bit_flips": sum(a[3] for a in attempts) if attempts and not external else None,
        "ldpc_checks_before": sum(a[4] for a in attempts) if attempts and not external else None,
        "ldpc_checks_after": sum(a[5] for a in attempts) if attempts and not external else None,
        "ldpc_residual_frames": sum(a[5] > 0 for a in attempts) if attempts and not external else None,
        "bch_corrected_bits": sum(max(a[6], 0) for a in attempts) if attempts else None,
        "bch_failed_frames": sum(a[6] < 0 for a in attempts) if attempts else None,
        "stream": ("gcs" if frames[0]["stream_type"] == 1 else "ts") if frames else None,
        "payload_bytes": len(payload),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "status_complete": bool(attempts),
    }
    # CW, silence and acquisition can produce arbitrary failed FEC hypotheses.
    # Keep whole-capture totals above; assess the recovered interval separately.
    accepted = [i for i, a in enumerate(attempts) if a[6] >= 0]
    report["payload_fec"] = None
    if accepted:
        first, last = accepted[0], accepted[-1]
        interval = attempts[first:last + 1]
        report["payload_fec"] = {
            "first_attempt": first, "last_attempt": last,
            "attempted_frames": len(interval),
            "bch_failed_frames": sum(a[6] < 0 for a in interval),
            "bch_corrected_bits": sum(max(a[6], 0) for a in interval),
            "ldpc_bit_flips": sum(a[3] for a in interval) if not external else None,
            "failed_before": sum(a[6] < 0 for a in attempts[:first]),
            "failed_after": sum(a[6] < 0 for a in attempts[last + 1:]),
        }
    if not attempts:
        ldpc = [int(n) for n in re.findall(r"LDPCCORR = (-?\d+)", status)]
        bch = [int(n) for n in re.findall(r"BCHCORR = (-?\d+)", status)]
        report.update(ldpc_bit_flips=sum(ldpc) if ldpc and not external else None,
                      bch_corrected_bits=sum(max(n, 0) for n in bch) if bch else None,
                      bch_failed_frames=sum(n < 0 for n in bch) if bch else None,
                      fec_frames=len(bch) if bch else None,
                      status_complete=not external and bool(bch) and len(ldpc) == len(bch))
    if external:
        drains = [tuple(map(int, m)) for m in HELPER_STATUS.findall(status)]
        balanced = (bool(soft) and not hard and len(drains) == 1 and
                    status.count("S2SOFT") == len(soft) and
                    status.count("S2HELPER") == 1 and
                    drains[0] == (len(soft), len(soft), 0, 0))
        report.update(fec_backend="external-soft", status_complete=balanced,
                      helper={"records": len(drains), "drained": balanced,
                              "sent": drains[-1][0] if drains else None,
                              "received": drains[-1][1] if drains else None,
                              "pending": drains[-1][2] if drains else None,
                              "exit": drains[-1][3] if drains else None},
                      helper_requirement=HELPER_REQUIREMENT)
    # fd-bbf contains valid headers only. Count receiver-side rejection messages too.
    report["bbheader_rejected_frames"] = status.count("Bad bbframe")
    # fd-info FREQ is in Hz (LeanDVB scales its normalized estimate by Fs).
    # The pinned DVB-S2 receiver fits fd-info MER from known PLHEADER symbols,
    # not the payload constellation.
    # Exclude unlocked acquisition, silence and the station-ID windows.
    locked, metrics = False, {"MER": [], "FREQ": []}
    for line in status.splitlines():
        if re.fullmatch(r"FRAMELOCK [01]", line):
            locked = line.endswith("1")
        match = re.fullmatch(r"(MER|FREQ) ([-+\d.eE]+)", line)
        if locked and match:
            value = float(match[2])
            if math.isfinite(value):
                metrics[match[1]].append(value)
    for token, name in (("MER", "mer_db"), ("FREQ", "frequency_offset_hz")):
        values = metrics[token]
        report[name] = ({"count": len(values), "min": min(values),
                         "median": statistics.median(values), "max": max(values),
                         "last": values[-1]} if values else None)
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


def ldpc_options(helper=None, modcod=None):
    """Pure argv for plans. Only the measured one-helper mode is exposed."""
    if helper is not None and (type(modcod) is not int or not 1 <= modcod <= 28):
        raise ValueError("external helper requires one known normal-frame MODCOD in 1..28")
    return (["--ldpc-helper", str(helper), "--nhelpers", "1"] if helper is not None
            else ["--ldpc-bf", "100"])


def resolve_ldpc_helper(helper):
    """LeanDVB execve does not search PATH; validate before opening a source."""
    if helper is None:
        return None
    found = shutil.which(str(helper))
    if not found:
        raise FileNotFoundError(f"LDPC helper not executable: {helper}. {HELPER_REQUIREMENT}")
    return str(Path(found).resolve())


def stop_decoder(process, timeout=3):
    """Reap a decoder started in its own session and stop its helper children."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        pass
    finally:
        # The parent may have exited while a helper still owns the group.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


# Kbch (Tables 5a, 5b) by MODCOD; short frames exist for QPSK 4..10 and 8PSK 12..16.
KBCH = {4: 32208, 5: 38688, 6: 43040, 7: 48408, 8: 51648,
        9: 53840, 10: 57472, 11: 58192, 12: 38688, 13: 43040,
        14: 48408, 15: 53840, 16: 57472, 17: 58192}
KBCH_SHORT = {4: 7032, 5: 9552, 6: 10632, 7: 11712, 8: 12432, 9: 13152, 10: 14232,
              12: 9552, 13: 10632, 14: 11712, 15: 13152, 16: 14232}


def receiver_command(executable, fs, rs, alpha, bb_fd, *, tune=0.0, rrc_rej=20,
                     ldpc_helper=None, modcod=None, short=False):
    """Shared executable argv, with a placeholder BB descriptor allowed for plans.

    LeanDVB searches for the next PLHEADER within +-round(sr_tol * slots), so the default
    100 ppm gives a 3,240-symbol dummy frame a zero-symbol window. Two separate crystals
    (~25 ppm apart here) then slip timing across runs of dummies; 500 ppm gives +-2 symbols.
    """
    command = [str(executable), "--standard", "DVB-S2", "--f32", "-f", str(fs),
               "--sr", str(rs), "--sampler", "rrc", "--rrc-rej", str(rrc_rej), "--fastdrift",
               "--sr-tol", "0.0005", "--roll-off", str(alpha)]
    command += ldpc_options(ldpc_helper, modcod)
    if ldpc_helper is not None:
        command += ["--modcods", str(1 << modcod), "--framesizes", "2" if short else "1"]
    elif short:
        command += ["--framesizes", "2"]
    command += ["--fd-bbf", str(bb_fd), "--fd-info", "2", "-d", "-d"]
    if tune:
        command += ["--tune", str(tune)]
    return command


def decode(capture, *, sample_rate=None, symbol_rate=None, alpha=None,
           receiver=None, expected=None, output=None, log=None, tune=0.0, timeout=120,
           rrc_rej=20, bb_output=None, ldpc_helper=None, modcod=None, short=False):
    """Decode one file, returning a JSON-serializable report and recovered bytes.

    LDPC counts are bit flips and unsatisfied parity checks, not a measured BER.
    BCH failures drop frames. FRAMELOCK is PL synchronization, also valid for GCS;
    LeanDVB's TS-only LOCK signal is deliberately not used for GCS.
    External helpers require one known normal-frame MODCOD; hard defaults stay unrestricted.
    """
    path, fs, rs, alpha = capture_info(capture, sample_rate, symbol_rate, alpha)
    executable = str(receiver or os.environ.get("LEANDVB", DEFAULT_RECEIVER))
    if not shutil.which(executable):
        raise FileNotFoundError(f"LeanDVB not found: {executable}; see examples/self-check/README.md")
    if not math.isfinite(tune) or timeout <= 0 or not math.isfinite(rrc_rej) or rrc_rej <= 0:
        raise ValueError("tune must be finite; timeout and RRC rejection must be positive")
    ldpc_options(ldpc_helper, modcod)
    helper = resolve_ldpc_helper(ldpc_helper)
    with tempfile.TemporaryFile() as bb, tempfile.TemporaryFile() as ts, \
            tempfile.TemporaryFile() as diagnostics, path.open("rb") as source:
        command = receiver_command(executable, fs, rs, alpha, bb.fileno(), tune=tune,
                                   rrc_rej=rrc_rej, ldpc_helper=helper, modcod=modcod, short=short)
        if helper is None:
            returncode = subprocess.run(command, stdin=source, stdout=ts, stderr=diagnostics,
                                        pass_fds=(bb.fileno(),), timeout=timeout, check=False).returncode
        else:
            process = subprocess.Popen(command, stdin=source, stdout=ts, stderr=diagnostics,
                                       pass_fds=(bb.fileno(),), start_new_session=True)
            try:
                returncode = process.wait(timeout=timeout)
            finally:
                stop_decoder(process)
                if log:
                    diagnostics.seek(0)
                    Path(log).write_text(diagnostics.read().decode("utf-8", errors="replace"))
        diagnostics.seek(0)
        status = diagnostics.read().decode("utf-8", errors="replace")
        if log:
            Path(log).write_text(status)
        if returncode:
            raise RuntimeError(f"LeanDVB exited {returncode}: {status[-2000:]}")
        bb.seek(0)
        raw_bb = bb.read()
        frames = parse_bbframes(raw_bb)
        if bb_output:
            Path(bb_output).write_bytes(raw_bb)
        if len({f["stream_type"] for f in frames}) > 1:
            raise ValueError("mixed GCS/TS captures require separate output streams")
        if frames and frames[0]["stream_type"] == 3:
            ts.seek(0)
            payload = ts.read()
            if len(payload) % 188 or any(payload[k] != 0x47 for k in range(0, len(payload), 188)):
                raise ValueError("receiver emitted an invalid TS packet stream")
        else:
            payload = b"".join(f["payload"] for f in frames)
    report = summarize(status, frames, payload, expected, ldpc_helper=helper)
    report.update({"sample_rate": fs, "symbol_rate": rs, "rolloff": alpha,
                   "rrc_rej_db": rrc_rej, "command": command})
    if helper is not None:
        report.update(ldpc_helper=helper, nhelpers=1,
                      ldpc_helper_sha256=hashlib.sha256(Path(helper).read_bytes()).hexdigest(),
                      receiver_sha256=hashlib.sha256(Path(shutil.which(executable)).read_bytes()).hexdigest())
    if output:
        Path(output).write_bytes(payload)
    return report, payload


def payload_sequence(pattern, frames, callsign, hold_frames=1, short=False):
    """Yield (MODCOD, payload) with one continuous firmware PRNG across frames."""
    import morse
    kbch = KBCH_SHORT if short else KBCH
    if not pattern or any(mc not in kbch for mc in pattern) or frames < 1 or hold_frames < 1:
        raise ValueError("need MODCODs 4..17, positive frame count and hold_frames")
    header = (("ESP-DVBS2 ID=" + morse.callsign(callsign)).ljust(39).encode("ascii") + b"\n"
              if callsign else None)
    state = 2026
    for frame in range(frames):
        mc = pattern[(frame // hold_frames) % len(pattern)]
        data = bytearray((kbch[mc] - 80) // 8)
        for i in range(len(data)):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            data[i] = state & 255
        if header:
            data[:40] = header
        yield mc, bytes(data)


def payload(modcod, frames, callsign, *, pattern=None, hold_frames=1, short=False):
    """Firmware test payload. No FEC model or receiver code is used here."""
    return b"".join(data for _, data in payload_sequence(
        pattern if pattern is not None else (modcod,), frames, callsign, hold_frames, short))


def vcm_match(frames, modcods, *, pattern, hold_frames, max_frames, callsign):
    """Strict interval verdict plus exact per-frame diagnostics for failed runs.

    All recovered frames must match full BBHEADERs, data and PLS in order.
    Diagnostics retain exact segments and interior gaps without accepting them.
    """
    result = dict(payload_match=False, host_model_exact=False, contiguous=False,
                  first_frame=None, last_frame=None, compared_bytes=0, compared_bits=0,
                  pattern_complete=set(modcods) == set(pattern),
                  exact_individual_frames=0, individual_compared_bytes=0,
                  missing_frames=0, segments=[], first_mismatch=None)
    if not frames or len(frames) != len(modcods):
        result["first_mismatch"] = dict(reason="missing_frames_or_pls")
        return result
    by_digest = {}
    for i, frame in enumerate(frames):
        by_digest.setdefault(hashlib.sha256(frame["payload"]).digest(), []).append(i)
    indices = [None] * len(frames)
    payload_indices = [None] * len(frames)
    for index, (mc, data) in enumerate(payload_sequence(pattern, max_frames, callsign, hold_frames)):
        candidates = by_digest.get(hashlib.sha256(data).digest(), ())
        if not candidates:
            continue
        header = b"\x60\0\0\0" + (len(data)*8).to_bytes(2, "big") + b"\0\0\0"
        header += bytes((crc8(header),))
        for i in candidates:
            if frames[i]["payload"] == data:
                payload_indices[i] = index
                if frames[i]["header"] == header and modcods[i] == mc:
                    indices[i] = index
    segments = result["segments"]
    for i, index in enumerate(indices):
        if index is None:
            if result["first_mismatch"] is None:
                result["first_mismatch"] = dict(recovered_frame=i, model_frame=payload_indices[i],
                    reason="payload_not_found" if payload_indices[i] is None else "header_or_pls")
            continue
        result["exact_individual_frames"] += 1
        result["individual_compared_bytes"] += len(frames[i]["payload"])
        previous = indices[i-1] if i else None
        if i and previous is not None and index != previous+1:
            if index > previous+1:
                result["missing_frames"] += index-previous-1
            if result["first_mismatch"] is None:
                result["first_mismatch"] = dict(recovered_frame=i, previous_model_frame=previous,
                    model_frame=index, reason="gap" if index > previous+1 else "repeat_or_reorder")
        if segments and segments[-1]["last_recovered_frame"] == i-1 and previous is not None and index == previous+1:
            segments[-1].update(last_recovered_frame=i, last_frame=index)
            segments[-1]["compared_bytes"] += len(frames[i]["payload"])
        else:
            segments.append(dict(first_recovered_frame=i, last_recovered_frame=i,
                                 first_frame=index, last_frame=index, compared_bytes=len(frames[i]["payload"])))
    if len(segments) == 1 and result["exact_individual_frames"] == len(frames):
        count = result["individual_compared_bytes"]
        result.update(payload_match=True, host_model_exact=True, contiguous=True,
                      first_frame=indices[0], last_frame=indices[-1],
                      compared_bytes=count, compared_bits=8*count)
    return result


def vcm_checks(report):
    """Strict recovered-data verdict; capture edges retain separate diagnostics.

    Exact BBHEADER/data and ordered PLS matching must already have succeeded.
    Failed FEC attempts *between* recovered frames still fail this check.
    This does not establish transmitter throughput or absence of dummy frames.
    """
    interval = report.get("payload_fec")
    return dict(frame_lock=bool(report.get("frame_lock")),
                fec_status_complete=bool(report.get("status_complete") and interval),
                host_model_exact=bool(report.get("host_model_exact")),
                contiguous=bool(report.get("contiguous")),
                pattern_complete=bool(report.get("pattern_complete")),
                bbheaders_valid=report.get("bbheader_rejected_frames") == 0,
                payload_fec_valid=bool(interval and interval["bch_failed_frames"] == 0))


def live_vcm_match(frames, modcods, expected, controls):
    """Match USB DATA FIELDs against confirmed sent bytes, including live switches.

    Controls are ordered PV writes with payload byte offsets. They are consumed
    while filling a frame; their MODCOD takes effect at the next frame boundary.
    Acquisition/EOF loss is allowed, but every scheduled run must be recovered.
    """
    sizes = {mc: (kbch-80)//8 for mc, kbch in enumerate(
        (32208, 38688, 43040, 48408, 51648, 53840, 57472, 58192,
         38688, 43040, 48408, 53840, 57472, 58192), 4)}
    result = dict(payload_match=False, host_model_exact=False, contiguous=False,
                  pattern_complete=False, control_boundaries_valid=False,
                  compared_bytes=0, compared_bits=0, segments=[], first_mismatch=None)
    if not frames or len(frames) != len(modcods) or not controls:
        result['first_mismatch'] = dict(reason='missing_frames_pls_or_controls')
        return result
    got = b''.join(f['payload'] for f in frames)
    at = expected.find(got) if got else -1
    headers_ok = True
    for i, (frame, mc) in enumerate(zip(frames, modcods)):
        size = sizes.get(mc, 0)
        header = b'\x60\0\0\0' + (size*8).to_bytes(2, 'big') + b'\0\0\0'
        header += bytes((crc8(header),))
        if not size or len(frame['payload']) != size or frame['header'] != header:
            headers_ok = False
            if result['first_mismatch'] is None:
                result['first_mismatch'] = dict(reason='header_or_pls', recovered_frame=i)
    result['payload_match'] = result['contiguous'] = at >= 0
    result['host_model_exact'] = at >= 0 and headers_ok
    if at < 0:
        result['first_mismatch'] = dict(reason='payload_not_one_contiguous_sent_slice')
    offset = at if at >= 0 else None
    for i, (frame, mc) in enumerate(zip(frames, modcods)):
        size = len(frame['payload'])
        segments = result['segments']
        if not segments or segments[-1]['modcod'] != mc:
            segments.append(dict(modcod=mc, mode=MODCODS.get(mc, str(mc)), frames=0,
                first_recovered_frame=i, last_recovered_frame=i,
                first_payload_byte=offset, end_payload_byte=offset))
        segment = segments[-1]
        segment['frames'] += 1
        segment['last_recovered_frame'] = i
        if offset is not None:
            offset += size
            segment['end_payload_byte'] = offset
    segments = result['segments']
    result['pattern_complete'] = [s['modcod'] for s in segments] == [c['modcod'] for c in controls]
    if result['pattern_complete']:
        for segment, control in zip(segments, controls):
            segment.update(control_sent_seconds=control.get('seconds'),
                           control_payload_byte=control['payload_bytes'])
        if at >= 0:
            result['control_boundaries_valid'] = all(
                0 <= s['first_payload_byte']-c['payload_bytes'] <= sizes[segments[i-1]['modcod']]
                for i, (s, c) in enumerate(zip(segments, controls)) if i)
    if result['host_model_exact']:
        result.update(compared_bytes=len(got), compared_bits=8*len(got),
                      first_payload_byte=at, end_payload_byte=at+len(got))
    return result


def stream_match(expected, got, per_frame):
    if not got or len(got) % per_frame:
        return None
    at = expected.find(got)
    while at >= 0 and at % per_frame:
        at = expected.find(got, at + 1)
    return at if at >= 0 else None


def stream_layout(modcod, frames, dummy_before=(), short=False):
    """Count complete data/dummy frames inside a nominal dump duration.

    Dummy positions are zero-based data-frame indices, repeated for a pair.
    A physical dummy has 3,330 symbols and produces no BBFRAME or payload.
    The final partial frame cannot be expected from a receiver.
    """
    if not 4 <= modcod <= (16 if short else 17) or (short and modcod == 11) or frames < 1:
        raise ValueError("need MODCOD 4..17 (short 4..10 and 12..16) and at least one nominal frame")
    if list(dummy_before) != sorted(dummy_before) or any(
            not isinstance(n, int) or not 0 <= n <= frames for n in dummy_before):
        raise ValueError("dummy positions must be ordered data-frame indices in 0..frames")
    symbols = (8370 if modcod < 12 else 5598) if short else 33282 if modcod < 12 else 22194
    left, data, dummies, at = frames * symbols, 0, 0, 0
    while left:
        dummy = at < len(dummy_before) and dummy_before[at] == data
        length = 3330 if dummy else symbols
        if left < length:
            break
        left -= length
        if dummy:
            at += 1
            dummies += 1
        else:
            data += 1
    return dict(expected_payload_frames=data, expected_dummy_frames=dummies,
                partial_payload_symbols=left if left and not dummy else 0,
                partial_dummy_symbols=left if left and dummy else 0)


def main():
    p = argparse.ArgumentParser(description="Decode board samples with a separate LeanDVB process.")
    p.add_argument("capture", type=Path)
    p.add_argument("--rate", "--sample-rate", dest="rate", type=float, required=True, help="samples/s")
    p.add_argument("--symbol-rate", type=float, required=True, help="symbols/s")
    p.add_argument("--rolloff", type=float, choices=(.20, .35), default=.35)
    p.add_argument("--scale", type=float, default=512, help="int16 full scale, board=512")
    p.add_argument("--format", choices=("i16", "s16", "cf32", "f32"), default="i16")
    p.add_argument("--modcod", type=int, default=4,
                   help="known normal-frame MODCOD for external helper (default 4)")
    p.add_argument("--stream", type=int, required=True, help="dump duration in nominal data-frame lengths")
    p.add_argument("--pattern", type=lambda value: tuple(map(int, value.split(","))), help="VCM MODCOD pattern")
    p.add_argument("--hold", type=int, default=1, help="VCM frames per pattern entry")
    p.add_argument("--dummy-before", type=lambda s: tuple(map(int, s.split(","))), default=(),
                   help="known dummy schedule: comma-separated data-frame indices, repeated for pairs")
    p.add_argument("--short", action="store_true",
                   help="short FECFRAMEs (QPSK 4..10, 8PSK 12..16): LeanDVB --framesizes 2, Kbch and symbol counts of Table 5b")
    p.add_argument("--callsign", default="N0CALL", help="callsign in the dumped payload")
    p.add_argument("--receiver", type=Path, help="leandvb executable, or set LEANDVB")
    p.add_argument("--ldpc-helper", type=Path, help=HELPER_REQUIREMENT)
    p.add_argument("--timeout", type=float, default=120, help="seconds allowed for LeanDVB")
    p.add_argument("--log", type=Path)
    p.add_argument("--output", type=Path, help="save recovered bytes")
    a = p.parse_args()
    a.format = {"s16": "i16", "f32": "cf32"}.get(a.format, a.format)
    try:
        if a.ldpc_helper is not None and a.pattern:
            raise ValueError("external helper mode requires --modcod, without --pattern")
        import numpy as np
        if a.pattern and (not 1 <= len(a.pattern) <= 16 or any(mc not in range(4, 18) for mc in a.pattern)
                          or not 1 <= a.hold <= 65535 or a.stream < 1 or a.dummy_before):
            raise ValueError("need 1..16 MODCODs in 4..17, positive stream, hold 1..65535, no dummy schedule")
        if a.short and (a.pattern or a.ldpc_helper is not None or a.modcod not in KBCH_SHORT):
            raise ValueError("--short needs one MODCOD (QPSK 4..10, 8PSK 12..16), no --pattern or helper")
        expected = payload(a.modcod, a.stream, a.callsign, short=a.short) if not a.pattern else None
        layout = stream_layout(a.modcod, a.stream, a.dummy_before, a.short)
        with tempfile.TemporaryDirectory() as tmp:
            source = a.capture
            if a.format == "i16":
                if not math.isfinite(a.scale) or a.scale <= 0 or source.stat().st_size % 4:
                    raise ValueError("need positive scale and complete int16 I/Q pairs")
                raw = np.fromfile(source, dtype="<i2").reshape(-1, 2) / a.scale
                source = Path(tmp) / "samples.cfile"
                # Receiver lookahead only. Never included in a dump's CRC.
                np.r_[raw[:, 0] + 1j*raw[:, 1], np.zeros(4096)].astype("<c8").tofile(source)
            report, got = decode(source, sample_rate=a.rate, symbol_rate=a.symbol_rate,
                                 alpha=a.rolloff, receiver=a.receiver, output=a.output, log=a.log, timeout=a.timeout,
                                 ldpc_helper=a.ldpc_helper, modcod=a.modcod, short=a.short,
                                 **({"bb_output": Path(tmp) / "frames.bbf"} if a.pattern else {}))
            if a.pattern:
                frames = parse_bbframes((Path(tmp) / "frames.bbf").read_bytes())
                report.update(vcm_match(frames, report["frame_modcods"], pattern=a.pattern,
                                        hold_frames=a.hold, max_frames=a.stream, callsign=a.callsign))
                checks = vcm_checks(report)
                checks["dump_coverage"] = len(frames) >= max(1, a.stream-2)
                checks["dump_fec_clean"] = all(report.get(k) == 0 for k in (
                    "ldpc_bit_flips", "bch_corrected_bits", "bch_failed_frames"))
                report.update(passed=all(checks.values()), checks=checks,
                              payload_bit_errors=0 if report["host_model_exact"] else None,
                              pattern=list(a.pattern), hold_frames=a.hold,
                              receiver_flush_samples=4096 if a.format == "i16" else 0)
                report.pop("command", None)
                print(json.dumps(report, sort_keys=True, allow_nan=False))
                return 0 if report["passed"] else 1
        size = len(expected) // a.stream
        at = stream_match(expected, got, size)
        # Count nonempty decoded BBFRAMEs, never nominal sample intervals or
        # dummy PLFRAMEs. Require all recovered bytes to belong to those frames.
        if len(got) != report["payload_frames"] * size:
            at = None
        report.update(stream_frames=a.stream, payload_match=at is not None,
                      first_frame=None if at is None else at // size,
                      **layout,
                      payload_bit_errors=None if at is None else 0,
                      compared_bits=0 if at is None else len(got)*8,
                      receiver_flush_samples=4096 if a.format == "i16" else 0)
        report.pop("command", None)
        print(json.dumps(report, sort_keys=True, allow_nan=False))
        return 0 if report["frame_lock"] and report["payload_match"] and report["status_complete"] else 1
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"error": str(exc)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
