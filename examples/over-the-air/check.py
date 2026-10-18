#!/usr/bin/env python3
"""Coordinate a contained SDR/S3 RF check, or decode a saved USB dump offline.

No hardware access for --help, --plan, --dry-run, or a missing --key.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(ROOT / "examples/live-video"))
import decode
from receive import stop

RADIOCONDA = str(Path.home() / "radioconda/bin/python")
DEFAULT_RX = "/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb"
MODES = {7: "QPSK 3/4", 13: "8PSK 2/3", 11: "QPSK 9/10", 15: "8PSK 5/6", 17: "8PSK 9/10"}
MODES.update({4: "QPSK 1/2", 6: "QPSK 2/3"})
RATES = {"1m": 1e6, "5m": 5e6, "10m": 10e6, "20m": 20e6,
         "26m667": 80e6 / 3, "33m333": 100e6 / 3}


def vcm_command(a):
    return f"vcm {a.hold} " + " ".join(map(str, a.pattern))


def settings(a):
    if a.pattern:
        if (not 1 <= len(a.pattern) <= 16 or any(mc not in range(4, 18) for mc in a.pattern)
                or not 1 <= a.hold <= 65535):
            raise ValueError("need 1..16 MODCODs in 4..17 and --hold 1..65535")
        mixed = any(mc < 12 for mc in a.pattern) and any(mc >= 12 for mc in a.pattern)
        if mixed and a.rate == "10m":
            raise ValueError("mixed VCM 10m exceeds encoder budget; use 5m (automatic axes)")
        if (a.rate not in ("1m", "5m", "10m") or (mixed and a.rate != "5m")
                or (a.rate == "1m" and any(mc >= 12 for mc in a.pattern))):
            raise ValueError("VCM needs 1m QPSK or 5m/10m same modulation; mixed needs 5m")
        a.modcod = a.pattern[0]
    rs = RATES[a.rate]
    fs = 2 * rs
    if not 2390 <= a.mhz <= 2450 or not 1 <= a.seconds <= 60:
        raise ValueError("need 2390..2450 MHz and 1..60 seconds")
    if not 0 <= a.gain <= 17 or not 16 <= a.peak <= 508:
        raise ValueError("need TX gain 0..17 and peak 16..508")
    if not math.isfinite(a.rx_gain) or not 0 <= a.rx_gain <= 76:
        raise ValueError("need finite RX gain 0..76 dB")
    if a.frames < 1 or a.min_frames < 1 or not math.isfinite(a.timeout) or a.timeout <= 0:
        raise ValueError("frame counts and decode timeout must be positive")
    if a.dump_rate is not None and (not math.isfinite(a.dump_rate) or a.dump_rate < 2 * rs):
        raise ValueError("USB dump rate must be finite and >= 2 samples/symbol")
    if a.dry_run is not None and (not a.callsign or a.dump_rate is None):
        raise ValueError("--dry-run requires --callsign and --dump-rate (after dump decimation)")
    if a.dry_run is not None and a.key:
        raise ValueError("--dry-run and --key are mutually exclusive")
    if a.dry_run is not None and (a.burst or a.upsample != 1):
        raise ValueError("--burst and --upsample apply to RF capture, not USB replay")
    if a.rate == "1m" and a.modcod >= 12:
        raise ValueError("firmware 8PSK requires a named rate >= 5m")
    if a.backend == "libiio":
        if a.args:
            raise ValueError("use --uri for libiio, not UHD --args")
        if a.uri is not None and (not a.uri.startswith(("ip:", "usb:")) or not a.uri.split(":", 1)[1].strip()):
            raise ValueError("need an explicit ip:HOST or usb:BUS.ADDR URI (no discovery)")
        fs = a.sample_rate if a.sample_rate is not None else max(2.5e6, min(61.44e6, 2 * rs))
        bw = a.bandwidth if a.bandwidth is not None else round(min(56e6, 1.35 * rs * 1.15))
        if not math.isfinite(fs) or not 2.5e6 <= fs <= 61.44e6 or fs < 1.35 * rs:
            raise ValueError("libiio sample rate must cover the signal and be 2.5..61.44 MS/s")
        if not math.isfinite(bw) or not 1.35 * rs <= bw <= 56e6:
            raise ValueError("libiio bandwidth must cover the signal and be <= 56 MHz")
        if not 1 <= a.upsample <= 8 or fs * a.upsample < 2 * rs:
            raise ValueError("need --upsample 1..8 and >= 2 decoder samples/symbol")
        if not 0 <= a.burst <= 16_000_000:
            raise ValueError("--burst must be 1..16000000 samples, or 0 for streaming")
        if not a.burst and fs > 10e6:
            raise ValueError("libiio streaming above 10 MS/s is refused; use --burst 16000000")
        if not math.isfinite(a.burst_margin) or a.burst_margin < 2:
            raise ValueError("--burst-margin must be finite and >= 2 seconds")
        return dict(pattern=a.pattern, hold_frames=a.hold, backend="libiio", uri=a.uri, sample_rate=fs, symbol_rate=rs,
                    bandwidth=bw, occupied_hz=1.35 * rs, frequency=a.mhz * 1e6,
                    gain=a.rx_gain, wire_format="sc16_le", file_format="cf32_le",
                    burst_samples=a.burst, burst_margin=a.burst_margin, upsample=a.upsample,
                    pre_seconds=1, post_seconds=1, capture_limit_seconds=a.seconds + 12)
    if (a.rate not in ("5m", "10m") or a.uri or a.sample_rate is not None or
            a.bandwidth is not None or a.burst or a.upsample != 1):
        raise ValueError("UHD supports 5m/10m; URI, sample rate, bandwidth, burst and upsample require --backend libiio")
    # Pin the RFIC clock, avoiding automatic selection and fractional DDC rates.
    # One RX channel, 40 MHz MCR / 4 or / 2, 10 or 20 MHz analog bandwidth.
    device_args = a.args or "addr=E200_IP"
    fields = dict(part.split("=", 1) for part in device_args.split(",") if "=" in part)
    if a.args is not None and not fields.get("addr", "").strip():
        raise ValueError("--args must include an explicit addr= (no device discovery)")
    if "master_clock_rate" in fields and float(fields["master_clock_rate"]) != 40e6:
        raise ValueError("this check requires master_clock_rate=40000000")
    if "master_clock_rate" not in fields:
        device_args += ",master_clock_rate=40000000"
    return dict(pattern=a.pattern, hold_frames=a.hold, backend="uhd", args=device_args, sample_rate=fs, symbol_rate=rs, master_clock_rate=40e6,
                bandwidth=fs, occupied_hz=1.35 * rs, frequency=a.mhz * 1e6,
                gain=a.rx_gain, antenna=a.antenna, wire_format="sc16", file_format="cf32_le",
                pre_seconds=1, post_seconds=1, capture_limit_seconds=a.seconds + 12)


def tx_command(a):
    rate = "" if a.rate == "1m" else a.rate + " "
    return f"dvb {a.mhz} {a.modcod} {rate}{a.seconds} {a.gain} {a.peak}" + (" --phy-solo" if a.phy_solo else "")


def capture_command(a, settings_path):
    iio = a.backend == "libiio"
    return [a.iio_python if iio else a.uhd_python,
            str(Path(__file__).with_name("capture_iio.py" if iio else "capture.py")),
            str(settings_path), "--key"]


def capture_bytes(s):
    samples = s.get("burst_samples") or s["capture_limit_seconds"] * s["sample_rate"]
    return int(samples * 8)


def plan(a, s):
    width = s["occupied_hz"] / 1e6
    result = dict(mode=MODES.get(a.modcod, f"MODCOD {a.modcod}"), rf=s,
                occupied_mhz=[a.mhz - width / 2, a.mhz + width / 2],
                board_command=tx_command(a), port=a.port,
                key_authorized=a.key, receiver=a.receiver,
                capture_command=capture_command(a, "OUT/settings.json"),
                decoder_command=decode.receiver_command(a.receiver, (a.dump_rate or s["sample_rate"]) * a.upsample,
                                                       s["symbol_rate"], .35, "BB_FD"),
                max_capture_bytes=capture_bytes(s),
                steps=[
                    "Require --key and explicit E200 addr; create a new evidence directory.",
                    "Reserve the board port without reset; info: refuse missing/invalid stored callsign.",
                    "Check optional --callsign matches storage and duration leaves room for both Morse IDs.",
                    "spiinput off: select synthetic xorshift32 GCS payload with stored callsign header.",
                    f"Start {a.uhd_python} capture.py --key; internal clock; RX channel 0; manual gain.",
                    "Validate actual MCR, sample rate, bandwidth, frequency, gain, antenna and LO lock.",
                    "Wait for first clean IQ packet and one second of pre-roll; abort if UHD fails.",
                    tx_command(a),
                    "Wait for board done (timed key window includes IDs), then one second post-roll.",
                    "Stop capture; reject overflow/timeout/incomplete capture or unhealthy board status.",
                    "LeanDVB cf32 -> BBFRAMEs; compare every recovered byte to one contiguous synthetic slice.",
                    "Save report.json, payload.bin, leandvb.log, spectrum.png, board.jsonl and UHD evidence.",
                ], containment="Closed Faraday box OR cabled with attenuators only; see docs/transmitting.md",
                interrupt="Interrupting the host does not stop the firmware's timed transmission.")
    if a.backend == "libiio":
        result["steps"] = [
            "Require --key and explicit --uri; reserve board port, validate stored callsign and ID duration.",
            "spiinput off; start receive-only pylibiio worker at the explicit URI; no discovery or firmware changes.",
            "Set and read back TX LO powerdown=1, every TX gain=-89.75, every DDS raw=0; refuse otherwise.",
            "Set/read back RX LO, sample rate, bandwidth, manual gain; verify TX inhibits again before RX buffer.",
            "Burst: arm without a buffer; trigger after stored-callsign Morse duration + startup margin."
            if a.burst else "Streaming: require one second of IQ before keying; refuse worker failure.",
            tx_command(a),
            "Wait for healthy board done; streaming stops after post-roll; burst requires one complete device buffer.",
            "Save native-rate spectrum; optionally upsample offline; same LeanDVB exact-byte verification and report.",
            "Keep capture.json, iio.log and TX inhibit readbacks; no TX settings are restored.",
        ]
    if a.pattern:
        result.update(pattern=list(a.pattern), hold_frames=a.hold,
                      board_setup=["vcm axes off", vcm_command(a)],
                      board_cleanup=["vcm off", "vcm axes off"])
    if a.dry_run is not None:
        result.update(source=str(a.dry_run), dump_sample_rate=a.dump_rate, callsign=a.callsign,
                      steps=["Read saved USB int16 I/Q; scale by 512; append 4096 decoder lookahead samples.",
                             "Plot saved IQ spectrum using host/plotstyle.py.",
                             "Run the same LeanDVB and contiguous synthetic-payload comparison as RF.",
                             "Save report.json, payload.bin, leandvb.log and spectrum.png; no devices opened."])
    return result


def stored_callsign(rows, expected=None, seconds=30):
    import morse
    if len(rows) != 1 or "error" in rows[0] or not rows[0].get("callsign"):
        raise ValueError("refusing to key: no stored callsign (info)")
    call = morse.callsign(rows[0]["callsign"])
    if expected and call != morse.callsign(expected):
        raise ValueError("stored callsign differs from --callsign")
    if seconds < 2 * morse.duration(call) + 2:
        raise ValueError("duration must leave at least 2 s of DVB after both Morse IDs")
    return call


def board_result(rows, a, call):
    if len(rows) != 1:
        raise ValueError("expected one board DVB result")
    r = rows[0]
    expected = dict(cmd="dvb", status="ESP_OK", modcod=a.modcod, callsign=call,
                    symbol_rate=int(RATES[a.rate]),
                    payload_source="xorshift32", encoder_failed=False, underruns=0,
                    freq_mhz=a.mhz, seconds=a.seconds, gain=a.gain, peak_level=a.peak)
    if "error" in r or any(r.get(k) != v for k, v in expected.items()):
        raise ValueError("board mode/payload/status mismatch; see board.jsonl")
    if a.pattern:
        axes = a.rate == "5m" and any(mc < 12 for mc in a.pattern) and any(mc >= 12 for mc in a.pattern)
        if (r.get("vcm") is not True or r.get("vcm_axes") is not axes or
                (axes and r.get("fill_kernel") != "pie-axes5") or
                any(r.get(k) != 0 for k in ("fatal_late", "late_fills", "late_switches"))):
            raise ValueError("board VCM renderer/deadline mismatch; see board.jsonl")
    if a.phy_solo:
        state, after = r.get("session", {}), r.get("after", {})
        if (not r.get("phy_solo") or state.get("wifi_initialized") is not False or
                state.get("wifi_mode_result") != 0x3001 or state.get("phy_users") != 2 or
                state.get("wifi_irq_routes") != [0, 0] or state.get("irq_sampled_cores") != 3 or
                after.get("wifi_initialized") is not False or after.get("phy_users") != 0 or
                not 0 < r.get("wifi_deinit_at_us", 0) < state.get("at_us", 0)):
            raise ValueError("PHY-solo handoff/cleanup mismatch; see board.jsonl")
    if r.get("fatal_late", 0) or r.get("frames_played", 0) < 1:
        raise ValueError("board failed to play healthy DVB frames")
    if not isinstance(r.get("frames_encoded"), int) or r["frames_encoded"] < r["frames_played"]:
        raise ValueError("invalid board encoder frame count")
    return r["frames_encoded"]


def wait_ready(process, out, s):
    deadline = time.monotonic() + 30
    while True:
        if process.poll() is not None:
            raise RuntimeError("SDR worker failed before key; see capture.json and backend log")
        raw_iio = s.get("backend") == "libiio"
        ready = out / "ready.json"
        iq = out / ("capture.i16" if raw_iio else "capture.cfile")
        if ready.exists() and (s.get("burst_samples") or
                (iq.exists() and iq.stat().st_size >= s["pre_seconds"] * s["sample_rate"] * (4 if raw_iio else 8))):
            return
        if time.monotonic() >= deadline:
            raise TimeoutError("no SDR readiness within 30 s; board was not keyed")
        time.sleep(.05)


def check_burst_timing(report, row, settings, command_started, board_done):
    # Completion arrives after RF stops, so backdating by actual keyed duration
    # gives a conservative upper bound on when the leading ID ended. Command
    # submission gives the earliest possible RF start / trailing ID.
    keyed_duration = row["elapsed_samples"] / row["dac_rate"]
    id_duration = row["id_samples"] / row["dac_rate"]
    if (not all(math.isfinite(v) for v in (keyed_duration, id_duration,
            report["buffer_started_monotonic"], report["buffer_filled_monotonic"])) or
            not 0 < 2 * id_duration < keyed_duration or
            report["buffer_filled_monotonic"] < report["buffer_started_monotonic"]):
        raise RuntimeError("invalid burst / board timing evidence")
    if (report["samples"] != settings["burst_samples"] or
            report["buffer_started_monotonic"] < board_done - keyed_duration + id_duration):
        raise RuntimeError("burst timing not proven after leading Morse ID; increase --burst-margin")
    if report["buffer_filled_monotonic"] >= command_started + keyed_duration - id_duration:
        raise RuntimeError("burst timing not proven before trailing Morse ID; increase --seconds")
    report.update(burst_delay_seconds=settings["burst_delay_seconds"],
                  board_done_monotonic=board_done, after_leading_id=True, before_trailing_id=True)


def hardware(a, s, out):
    import board
    process = None
    def command(text, timeout=30):
        rows = board.run(a.port, text, timeout=timeout, reset=False)
        with (out / "board.jsonl").open("a") as sink:
            for row in rows:
                sink.write(json.dumps(dict(command=text, **row)) + "\n")
        if not rows or any("error" in row for row in rows):
            raise RuntimeError(f"board rejected {text}; see board.jsonl")
        return rows
    log = "iio.log" if a.backend == "libiio" else "uhd.log"
    with board.reserve_port(a.port), (out / log).open("w") as diagnostics:
        call = stored_callsign(command("info"), a.callsign, a.seconds)
        if s.get("burst_samples"):
            import morse
            delay = morse.duration(call) + s["burst_margin"]
            if delay + s["burst_samples"] / s["sample_rate"] >= a.seconds - morse.duration(call):
                raise ValueError("burst does not fit between both Morse IDs; increase --seconds")
            s["burst_delay_seconds"] = delay
        command("spiinput off")
        (out / "settings.json").write_text(json.dumps(s, indent=2) + "\n")
        try:
            if a.pattern:
                if command("vcm axes off")[-1].get("axes") is not False:
                    raise RuntimeError("board did not disable manual axis override")
                configured = command(vcm_command(a))[-1]
                if (configured.get("pattern") != list(a.pattern) or configured.get("hold_frames") != a.hold
                        or configured.get("enabled") is not True):
                    raise RuntimeError("board VCM configuration differs")
            process = subprocess.Popen(capture_command(a, out / "settings.json"),
                                       stdout=diagnostics, stderr=subprocess.STDOUT)
            wait_ready(process, out, s)
            print(f"Capture ready; stored callsign {call}; keying: {tx_command(a)}", flush=True)
            if s.get("burst_samples"):
                # Keep serial ownership in one command while the worker waits unbuffered.
                started = time.monotonic()
                with ThreadPoolExecutor(max_workers=1) as executor:
                    pending = executor.submit(command, tx_command(a), a.seconds + 10)
                    while time.monotonic() < started + s["burst_delay_seconds"]:
                        if pending.done():
                            pending.result()
                            raise RuntimeError("board finished before burst trigger")
                        if process.poll() is not None:
                            raise RuntimeError("IIO worker failed before burst trigger")
                        time.sleep(.02)
                    (out / "trigger").touch()
                    rows = pending.result()
                board_done = time.monotonic()
            else:
                rows = command(tx_command(a), a.seconds + 10)
                time.sleep(s["post_seconds"])
            (out / "stop").touch()
            code = process.wait(timeout=60 if a.backend == "libiio" else 7)
            capture_report = json.loads((out / "capture.json").read_text())
            if code or not capture_report["complete"] or capture_report["overflows"]:
                raise RuntimeError(f"SDR capture incomplete; see capture.json and {log}")
            if s.get("burst_samples"):
                check_burst_timing(capture_report, rows[0], s, started, board_done)
                (out / "capture.json").write_text(json.dumps(capture_report, indent=2) + "\n")
            return call, board_result(rows, a, call), capture_report
        finally:
            (out / "stop").touch()
            stop(process)  # Same bounded terminate/kill cleanup as live-video/receive.py.
            if a.pattern:
                cleanup = []
                for text, field in (("vcm off", "enabled"), ("vcm axes off", "axes")):
                    try:
                        if command(text)[-1].get(field) is not False:
                            raise RuntimeError("missing cleanup acknowledgement")
                        cleanup.append(dict(command=text, passed=True))
                    except Exception as exc:
                        cleanup.append(dict(command=text, passed=False, error=str(exc)))
                (out / "cleanup.json").write_text(json.dumps(cleanup, indent=2) + "\n")
                if not all(row["passed"] for row in cleanup):
                    raise RuntimeError("VCM cleanup failed; see cleanup.json")


def convert_dump(source, target):
    import numpy as np
    if not source.stat().st_size or source.stat().st_size % 4:
        raise ValueError("USB dump must contain complete int16 I/Q pairs")
    with source.open("rb") as stream, target.open("xb") as sink:
        while raw := stream.read(4 * 262144):
            values = np.frombuffer(raw, dtype="<i2").reshape(-1, 2) / 512
            (values[:, 0] + 1j * values[:, 1]).astype("<c8").tofile(sink)
        # Identical lookahead to host/decode.py; never part of dump CRC or spectrum.
        np.zeros(4096, dtype="<c8").tofile(sink)


def upsample(source, target, factor):
    """Offline polyphase interpolation, bounded memory and overlapping FIR context."""
    import numpy as np
    from scipy.signal import resample_poly
    z = np.memmap(source, mode="r", dtype="<c8")
    with target.open("xb") as sink:
        for first in range(0, len(z), 262144):
            end = min(first + 262144, len(z))
            # scipy's default FIR spans 10 input samples on either side for integer upsampling.
            left, right = max(0, first - 32), min(len(z), end + 32)
            chunk = resample_poly(z[left:right], factor, 1)
            chunk[(first-left)*factor:(end-left)*factor].astype("<c8").tofile(sink)


def spectrum(source, fs, s, out, flush_samples=0):
    import numpy as np
    from scipy.signal import welch
    import plotstyle as style
    z = np.memmap(source, mode="r", dtype="<c8")
    if flush_samples:
        z = z[:-flush_samples]
    n = min(16384, len(z))
    if n < 256:
        raise ValueError("capture too short for spectrum")
    starts = np.unique(np.linspace(0, len(z) - n, min(256, len(z) // n), dtype=int))
    psd = np.zeros(n)
    for start in starts:
        chunk = z[start:start+n]
        if not np.all(np.isfinite(chunk)):
            raise ValueError("nonfinite capture samples")
        freq, power = welch(chunk, fs=fs, window="hann", nperseg=n, noverlap=0,
                            return_onesided=False, detrend=False, scaling="density")
        psd += power / len(starts)
    fig, ax = style.plt.subplots(figsize=(style.WIDTH, 3.4))
    ax.plot(np.fft.fftshift(freq) / 1e6, 10 * np.log10(np.maximum(np.fft.fftshift(psd), 1e-30)))
    for edge in (-.5, .5):
        ax.axvline(edge * s["occupied_hz"] / 1e6, color=style.RED, linestyle="--", linewidth=.7)
    label = "USB dump, radio off" if flush_samples else f"RF capture at {s['frequency']/1e6:g} MHz"
    ax.set(xlabel="Baseband offset (MHz)", ylabel="PSD (dBFS/Hz)",
           title=f"{label}; {s['symbol_rate']/1e6:g} Msym/s; Fs={fs/1e6:g} MS/s")
    ax.grid()
    style.save_figure(fig, out / "spectrum.png",
                      "Hann PSD averaged across saved IQ; red: nominal RRC edges.\n"
                      "Digital full scale only; no calibrated RF power or emission-mask claim.")
    return dict(blocks=len(starts), fft_samples=n, units="dBFS/Hz",
                includes_id_and_silence=not bool(flush_samples) and not bool(s.get("burst_samples")))


def pattern_report(r, recovered, frames, call, a):
    r.update(decode.vcm_match(recovered, r["frame_modcods"], pattern=a.pattern,
                             hold_frames=a.hold, max_frames=frames, callsign=call))
    checks = decode.vcm_checks(r)
    checks["enough_frames"] = r["payload_frames"] >= a.min_frames
    if a.dry_run is not None:
        checks["dump_coverage"] = r["payload_frames"] >= a.frames - 2
        checks["dump_fec_clean"] = all(r.get(k) == 0 for k in (
            "ldpc_bit_flips", "bch_corrected_bits", "bch_failed_frames"))
    r.update(passed=all(checks.values()), checks=checks,
             failed_checks=[k for k, passed in checks.items() if not passed],
             pattern=list(a.pattern), hold_frames=a.hold, reference_frames=frames, callsign=call,
             payload_bit_errors=0 if r["host_model_exact"] else None,
             evidence="usb-dump" if a.dry_run is not None else "rf-capture")
    return r


def recheck(a):
    """Saved BBFRAME/log audit; never imports a device driver or changes evidence."""
    import hashlib
    source = a.recheck.resolve()
    if a.key or a.dry_run is not None:
        raise ValueError("--recheck cannot be combined with --key or --dry-run")
    if a.plan:
        print(json.dumps(dict(offline_recheck=str(source), devices=False)))
        return 0
    settings = json.loads((source / "settings.json").read_text())
    rows = [json.loads(line) for line in (source / "board.jsonl").read_text().splitlines()]
    results = [r for r in rows if r.get("cmd") == "dvb" and "status" in r]
    if len(results) != 1 or not settings.get("pattern"):
        raise ValueError("need one board result and saved VCM pattern")
    board = results[0]
    a.pattern = tuple(settings["pattern"])
    a.hold = settings["hold_frames"]
    a.modcod = a.pattern[0]
    a.rate = next(name for name, rate in RATES.items() if abs(rate-settings["symbol_rate"]) < 1)
    a.mhz, a.seconds, a.gain, a.peak = (board[k] for k in ("freq_mhz", "seconds", "gain", "peak_level"))
    a.phy_solo = board.get("phy_solo", False)
    call = board["callsign"]
    frames = decode.parse_bbframes((source / "frames.bbf").read_bytes())
    r = decode.summarize((source / "leandvb.log").read_text(), frames, b"".join(f["payload"] for f in frames))
    report = pattern_report(r, frames, board["frames_encoded"], call, a)
    capture = json.loads((source / "capture.json").read_text())
    cleanup = json.loads((source / "cleanup.json").read_text())
    try:
        board_result(results, a, call)
        board_ok = True
    except ValueError as exc:
        board_ok = False
        report["board_error"] = str(exc)
    report["checks"].update(board_valid=board_ok,
        capture_complete=capture.get("complete") is True and not capture.get("overflows"),
        cleanup_valid=([r.get("command") for r in cleanup] == ["vcm off", "vcm axes off"] and
                       all(r.get("passed") is True for r in cleanup)))
    report.update(passed=all(report["checks"].values()),
                  failed_checks=[k for k, passed in report["checks"].items() if not passed],
                  source=str(source), board=board, capture=capture,
                  source_sha256={name: hashlib.sha256((source/name).read_bytes()).hexdigest()
                      for name in ("frames.bbf", "leandvb.log", "board.jsonl", "settings.json", "capture.json", "cleanup.json")})
    out = (a.out or ROOT/"notes"/dt.datetime.now().strftime("ota-recheck-%Y%m%d-%H%M%S")).resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out/"report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("passed", "failed_checks", "exact_individual_frames",
        "individual_compared_bytes", "missing_frames", "first_mismatch", "segments")}))
    print(f"Evidence: {out}")
    return 0 if report["passed"] else 1


def analyze(source, fs, frames, call, a, s, out):
    if a.pattern:
        bb = out / "frames.bbf"
        r, _ = decode.decode(source, sample_rate=fs, symbol_rate=s["symbol_rate"], alpha=.35,
                             receiver=a.receiver, output=out / "payload.bin", bb_output=bb,
                             log=out / "leandvb.log", timeout=a.timeout)
        recovered = decode.parse_bbframes(bb.read_bytes())
        return pattern_report(r, recovered, frames, call, a)

    r, got = decode.decode(source, sample_rate=fs, symbol_rate=s["symbol_rate"], alpha=.35,
                           receiver=a.receiver, output=out / "payload.bin", log=out / "leandvb.log",
                           timeout=a.timeout)
    expected = decode.payload(a.modcod, frames, call)
    size = len(expected) // frames
    at = decode.stream_match(expected, got, size)
    r.update(payload_match=at is not None, payload_bit_errors=0 if at is not None else None,
             compared_bits=len(got) * 8 if at is not None else 0,
             first_frame=None if at is None else at // size, reference_frames=frames,
             payload_frames=len(got) // size, callsign=call,
             evidence="usb-dump" if a.dry_run is not None else "rf-capture")
    r["passed"] = bool(r["frame_lock"] and r["status_complete"] and r["payload_match"]
                       and r["modcods"] == [a.modcod] and r["bb_frames"] >= a.min_frames
                       and r["payload_frames"] == r["bb_frames"])
    return r


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("out", type=Path, nargs="?", help="new evidence directory (default notes/ota-TIMESTAMP)")
    p.add_argument("--plan", action="store_true")
    p.add_argument("--phy-solo", action="store_true", help="experimental Wi-Fi teardown during playback")
    p.add_argument("--key", action="store_true", help="explicitly authorize contained hardware run")
    p.add_argument("--recheck", type=Path, help="offline audit of saved VCM BBFRAMEs/logs; no IQ or devices")
    p.add_argument("--dry-run", type=Path, metavar="USB_DUMP", help="offline int16 IQ, never opens devices")
    p.add_argument("--dump-rate", type=float, help="saved USB samples/s after decimation")
    p.add_argument("--frames", type=int, default=16, help="synthetic frames in USB dump")
    p.add_argument("--callsign", help="dump callsign; optional stored-callsign cross-check for RF")
    p.add_argument("--backend", choices=("uhd", "libiio"), default="uhd")
    p.add_argument("--uri", help="explicit libiio URI, e.g. ip:192.168.2.1 or usb:1.2.3")
    p.add_argument("--sample-rate", type=float, help="libiio samples/s (default >=2 samples/symbol, max 61.44e6)")
    p.add_argument("--bandwidth", type=float, help="libiio RX analog bandwidth in Hz; must cover occupied signal")
    p.add_argument("--burst", type=int, default=0, metavar="SAMPLES", help="libiio single device buffer after CW ID; 0 streams")
    p.add_argument("--burst-margin", type=float, default=2.3, help="startup margin beyond stored-callsign CW duration, seconds")
    p.add_argument("--upsample", type=int, default=1, help="libiio offline integer interpolation before decoding (1..8)")
    p.add_argument("--iio-python", default=RADIOCONDA, help="Python with pylibiio (Radioconda includes it)")
    p.add_argument("--args", help="explicit UHD args including addr=E200_IP")
    p.add_argument("--port", default="/dev/cu.usbmodem101")
    p.add_argument("--mhz", type=int, default=2420)
    p.add_argument("--modcod", type=int, choices=MODES, default=7)
    p.add_argument("--rate", choices=RATES, default="5m")
    p.add_argument("--pattern", type=lambda value: tuple(map(int, value.split(","))),
                   help="VCM MODCOD pattern; first entry selects starting MODCOD")
    p.add_argument("--hold", type=int, default=4, help="VCM frames per pattern entry")
    p.add_argument("--seconds", type=int, default=30, help="keyed window INCLUDING both IDs")
    p.add_argument("--gain", type=int, default=0, help="S3 vendor gain index")
    p.add_argument("--peak", type=int, default=128)
    p.add_argument("--rx-gain", type=float, default=0, help="E200 manual receive gain, dB")
    p.add_argument("--antenna", choices=("RX2", "TX/RX"), default="RX2")
    p.add_argument("--receiver", default=os.environ.get("RX", DEFAULT_RX))
    p.add_argument("--uhd-python", default=RADIOCONDA, help="Python with compatible E200 UHD bindings")
    p.add_argument("--timeout", type=float, default=600, help="offline decoder timeout, seconds")
    p.add_argument("--min-frames", type=int, default=8)
    return p


def main():
    p = parser()
    a = p.parse_args()
    try:
        if a.recheck is not None:
            return recheck(a)
        s = settings(a)
        if a.plan:
            print(json.dumps(plan(a, s), indent=2))
            return 0
        address = a.uri if a.backend == "libiio" else a.args
        if a.dry_run is None and (not a.key or not address):
            p.error("hardware requires explicit --key AND --uri (libiio) / --args addr= (UHD); use --plan or --dry-run")
        # Fail on missing software before importing board or starting the SDR.
        import numpy  # noqa: F401
        import scipy  # noqa: F401
        import plotstyle  # noqa: F401
        import morse
        if not shutil.which(a.receiver):
            raise ValueError(f"LeanDVB not found: {a.receiver}")
        if a.callsign:
            a.callsign = morse.callsign(a.callsign)
        worker_python = a.iio_python if a.backend == "libiio" else a.uhd_python
        if a.dry_run is None and not shutil.which(worker_python):
            raise ValueError(f"capture Python not found: {worker_python}")
        out = (a.out or ROOT / "notes" / dt.datetime.now().strftime("ota-%Y%m%d-%H%M%S")).resolve()
        out.mkdir(parents=True, exist_ok=False)
        (out / "plan.json").write_text(json.dumps(plan(a, s), indent=2) + "\n")
        (out / "revision.json").write_text(json.dumps(dict(
            started_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
            coordinator=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)),
            receiver=str(Path(shutil.which(a.receiver)).resolve())), indent=2) + "\n")
        print(f"Center {a.mhz} MHz; {MODES.get(a.modcod, a.modcod)} {a.rate}; occupied {s['occupied_hz']/1e6:g} MHz; "
              f"{a.mhz-s['occupied_hz']/2e6:g}..{a.mhz+s['occupied_hz']/2e6:g} MHz", flush=True)
        result = dict(passed=False, evidence="usb-dump" if a.dry_run else "rf-capture")
        try:
            if a.dry_run is not None:
                with tempfile.TemporaryDirectory(prefix="ota-usb-") as tmp:
                    source = Path(tmp) / "dump.cfile"
                    convert_dump(a.dry_run, source)
                    plotted = spectrum(source, a.dump_rate, s, out, flush_samples=4096)
                    result = analyze(source, a.dump_rate, a.frames, a.callsign, a, s, out)
                    result["spectrum"] = plotted
                    result["receiver_flush_samples"] = 4096
            else:
                disk_bytes = capture_bytes(s) * ((1.5 if a.backend == "libiio" else 1) + (a.upsample if a.upsample > 1 else 0))
                if shutil.disk_usage(out).free < disk_bytes + 100e6:
                    raise ValueError("insufficient free disk for bounded cf32 capture")
                call, frames, capture_report = hardware(a, s, out)
                source = out / "capture.cfile"
                # Render even if the receiver subsequently fails to acquire.
                result["spectrum"] = spectrum(source, s["sample_rate"], s, out)
                if a.upsample > 1:
                    target = out / "decode.cfile"
                    upsample(source, target, a.upsample)
                    source = target
                result.update(analyze(source, s["sample_rate"] * a.upsample, frames, call, a, s, out))
                result["capture"] = capture_report
                result["offline_upsample"] = a.upsample
        except (Exception, KeyboardInterrupt) as exc:
            result.update(passed=False, error=str(exc) or "interrupted; firmware retains its timed key window")
            source = out / "capture.cfile"
            if source.exists() and source.stat().st_size >= 2048 and "spectrum" not in result:
                try:
                    result["spectrum"] = spectrum(source, s["sample_rate"], s, out)
                except Exception as plot_error:
                    result["spectrum_error"] = str(plot_error)
        (out / "report.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
        print(json.dumps(result, allow_nan=False))
        print(f"Evidence: {out}")
        return 0 if result["passed"] else 1
    except (OSError, ValueError) as exc:
        p.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
