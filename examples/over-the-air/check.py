#!/usr/bin/env python3
"""Coordinate a contained E200/S3 RF check, or decode a saved USB dump offline.

No hardware access for --help, --plan, --dry-run, or a missing --key.
"""
import argparse
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

RADIOCONDA = "/Users/river/radioconda/bin/python"
DEFAULT_RX = "/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb"
MODES = {7: "QPSK 3/4", 13: "8PSK 2/3", 11: "QPSK 9/10", 15: "8PSK 5/6", 17: "8PSK 9/10"}


def settings(a):
    rs = {"5m": 5e6, "10m": 10e6}[a.rate]
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
    return dict(args=device_args, sample_rate=fs, symbol_rate=rs, master_clock_rate=40e6,
                bandwidth=fs, occupied_hz=1.35 * rs, frequency=a.mhz * 1e6,
                gain=a.rx_gain, antenna=a.antenna, wire_format="sc16", file_format="cf32_le",
                pre_seconds=1, post_seconds=1, capture_limit_seconds=a.seconds + 12)


def tx_command(a):
    return f"dvb {a.mhz} {a.modcod} {a.rate} {a.seconds} {a.gain} {a.peak}" + (" --phy-solo" if a.phy_solo else "")


def plan(a, s):
    width = s["occupied_hz"] / 1e6
    result = dict(mode=MODES[a.modcod], rf=s,
                occupied_mhz=[a.mhz - width / 2, a.mhz + width / 2],
                board_command=tx_command(a), port=a.port,
                key_authorized=a.key, receiver=a.receiver,
                capture_command=[a.uhd_python, str(Path(__file__).with_name("capture.py")),
                                 "OUT/settings.json", "--key"],
                decoder_command=decode.receiver_command(a.receiver, a.dump_rate or s["sample_rate"],
                                                       s["symbol_rate"], .35, "BB_FD"),
                max_capture_bytes=int(s["capture_limit_seconds"] * s["sample_rate"] * 8),
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
                    symbol_rate={"5m": 5000000, "10m": 10000000}[a.rate],
                    payload_source="xorshift32", encoder_failed=False, underruns=0,
                    freq_mhz=a.mhz, seconds=a.seconds, gain=a.gain, peak_level=a.peak)
    if "error" in r or any(r.get(k) != v for k, v in expected.items()):
        raise ValueError("board mode/payload/status mismatch; see board.jsonl")
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
            raise RuntimeError("UHD failed before key; see uhd.log and capture.json")
        ready, iq = out / "ready.json", out / "capture.cfile"
        if ready.exists() and iq.stat().st_size >= s["pre_seconds"] * s["sample_rate"] * 8:
            return
        if time.monotonic() >= deadline:
            raise TimeoutError("no clean UHD pre-roll within 30 s; board was not keyed")
        time.sleep(.05)


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
    with board.reserve_port(a.port), (out / "uhd.log").open("w") as diagnostics:
        call = stored_callsign(command("info"), a.callsign, a.seconds)
        command("spiinput off")
        (out / "settings.json").write_text(json.dumps(s, indent=2) + "\n")
        try:
            process = subprocess.Popen([a.uhd_python, str(Path(__file__).with_name("capture.py")),
                                        str(out / "settings.json"), "--key"],
                                       stdout=diagnostics, stderr=subprocess.STDOUT)
            wait_ready(process, out, s)
            print(f"Capture ready; stored callsign {call}; keying: {tx_command(a)}", flush=True)
            rows = command(tx_command(a), a.seconds + 10)
            time.sleep(s["post_seconds"])
            (out / "stop").touch()
            code = process.wait(timeout=5)
            capture_report = json.loads((out / "capture.json").read_text())
            if code or not capture_report["complete"] or capture_report["overflows"]:
                raise RuntimeError("UHD capture incomplete; see capture.json and uhd.log")
            return call, board_result(rows, a, call), capture_report
        finally:
            (out / "stop").touch()
            stop(process)  # Same bounded terminate/kill cleanup as live-video/receive.py.


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
    return dict(blocks=len(starts), fft_samples=n, units="dBFS/Hz", includes_id_and_silence=not bool(flush_samples))


def analyze(source, fs, frames, call, a, s, out):
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
    p.add_argument("--dry-run", type=Path, metavar="USB_DUMP", help="offline int16 IQ, never opens devices")
    p.add_argument("--dump-rate", type=float, help="saved USB samples/s after decimation")
    p.add_argument("--frames", type=int, default=16, help="synthetic frames in USB dump")
    p.add_argument("--callsign", help="dump callsign; optional stored-callsign cross-check for RF")
    p.add_argument("--args", help="explicit UHD args including addr=E200_IP")
    p.add_argument("--port", default="/dev/cu.usbmodem101")
    p.add_argument("--mhz", type=int, default=2420)
    p.add_argument("--modcod", type=int, choices=MODES, default=7)
    p.add_argument("--rate", choices=("5m", "10m"), default="5m")
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
        s = settings(a)
        if a.plan:
            print(json.dumps(plan(a, s), indent=2))
            return 0
        if a.dry_run is None and (not a.key or not a.args):
            p.error("hardware requires explicit --key AND --args addr=E200_IP; use --plan or --dry-run")
        # Fail on missing software before importing board or starting the SDR.
        import numpy  # noqa: F401
        import scipy  # noqa: F401
        import plotstyle  # noqa: F401
        import morse
        if not shutil.which(a.receiver):
            raise ValueError(f"LeanDVB not found: {a.receiver}")
        if a.callsign:
            a.callsign = morse.callsign(a.callsign)
        if a.dry_run is None and not shutil.which(a.uhd_python):
            raise ValueError(f"UHD Python not found: {a.uhd_python}")
        out = (a.out or ROOT / "notes" / dt.datetime.now().strftime("ota-%Y%m%d-%H%M%S")).resolve()
        out.mkdir(parents=True, exist_ok=False)
        (out / "plan.json").write_text(json.dumps(plan(a, s), indent=2) + "\n")
        (out / "revision.json").write_text(json.dumps(dict(
            started_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
            coordinator=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)),
            receiver=str(Path(shutil.which(a.receiver)).resolve())), indent=2) + "\n")
        print(f"Center {a.mhz} MHz; {MODES[a.modcod]} {a.rate}; occupied {s['occupied_hz']/1e6:g} MHz; "
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
                if shutil.disk_usage(out).free < s["capture_limit_seconds"] * s["sample_rate"] * 8 + 100e6:
                    raise ValueError("insufficient free disk for bounded cf32 capture")
                call, frames, capture_report = hardware(a, s, out)
                source = out / "capture.cfile"
                # Render even if the receiver subsequently fails to acquire.
                result["spectrum"] = spectrum(source, s["sample_rate"], s, out)
                result.update(analyze(source, s["sample_rate"], frames, call, a, s, out))
                result["capture"] = capture_report
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
