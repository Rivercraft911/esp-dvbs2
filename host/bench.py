#!/usr/bin/env python3
"""One recorded cabled bench run. --dry-run never opens the board or SDR.

An RF path and explicit transmit authorization are mandatory for a live run.
The board uses a finite tx/dvb command. On a capture error we still wait for
that command and save its records; killing the host process would not stop RF.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
import datetime as dt
import fcntl
import hashlib
import json
import math
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time
import uuid

import sdr

ROOT = pathlib.Path(__file__).resolve().parents[1]


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("mode", choices=("tx", "dvb"))
    p.add_argument("--topic", required=True, help="new results topic, e.g. monday-cable")
    p.add_argument("--attenuation", type=float, nargs="+", help="each attenuator in dB, in path order")
    p.add_argument("--cable", help="describe the complete path, connectors and RX1")
    p.add_argument("--radiates", choices=("yes", "no"), help="does any part of this setup radiate?")
    p.add_argument("--authorize-tx", action="store_true", help="authorize this timed licensed transmission")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--mhz", type=int, default=2420)
    p.add_argument("--seconds", type=int, default=10)
    p.add_argument("--gain", type=int, default=0, help="board vendor gain index, not dBm")
    p.add_argument("--peak", type=int, default=128, help="DVB DAC peak level")
    p.add_argument("--signal", choices=("tone", "tone128", "two"), default="tone128")
    p.add_argument("--modcod", type=int, default=4)
    p.add_argument("--settle", type=float, default=2.0, help="seconds after starting board.py")
    p.add_argument("--board-port", default="/dev/cu.usbmodem101")
    p.add_argument("--receiver", type=pathlib.Path, help="patched LeanDVB executable")
    sdr.add_arguments(p)
    return p


def rf_path(args) -> dict:
    interactive = sys.stdin.isatty()
    if args.attenuation is None and interactive:
        args.attenuation = [float(v) for v in input("Attenuators in path order, dB (e.g. 20 10): ").split()]
    if args.cable is None and interactive:
        args.cable = input("Cable path, connectors and receiver input: ").strip()
    if args.radiates is None and interactive:
        args.radiates = input("Does anything radiate? Enter yes or no: ").strip().lower()
    if not args.attenuation or not args.cable or not args.cable.strip() or args.radiates not in ("yes", "no"):
        raise ValueError("RF path required: --attenuation DB... --cable DESCRIPTION --radiates yes|no")
    if (any(not math.isfinite(v) or v < 0 for v in args.attenuation)
            or not math.isfinite(sum(args.attenuation)) or sum(args.attenuation) < 30):
        raise ValueError("record finite, nonnegative attenuators totaling at least 30 dB")
    if not args.dry_run and not args.authorize_tx and interactive:
        args.authorize_tx = input("Authorize this timed transmission under your amateur licence? Type TRANSMIT: ") == "TRANSMIT"
    if not args.dry_run and not args.authorize_tx:
        raise ValueError("transmit authorization required: pass --authorize-tx or confirm interactively")
    return {"attenuators_db": args.attenuation, "total_attenuation_db": sum(args.attenuation),
            "cable": args.cable, "radiates": args.radiates == "yes",
            "transmit_authorized": bool(args.authorize_tx), "source": "operator supplied"}


def board_command(command: str, args) -> list[str]:
    return [sys.executable, str(ROOT / "host/board.py"), command,
            "--port", args.board_port, "--timeout", str(args.seconds + 20)]


def make_plan(args, radio: sdr.SDR, path: dict) -> dict:
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", args.topic):
        raise ValueError("topic must be 1..64 lowercase letters, digits or hyphens")
    if not 2390 <= args.mhz <= 2450 or not 1 <= args.seconds <= 60 or not 0 <= args.gain <= 17:
        raise ValueError("board command requires 2390..2450 MHz, 1..60 s, gain 0..17")
    if not 4 <= args.modcod <= 11 or not 16 <= args.peak <= 508:
        raise ValueError("DVB requires MODCOD 4..11 and peak 16..508")
    if not math.isfinite(args.settle) or args.settle < 0:
        raise ValueError("settle must be finite and nonnegative")
    radio.read_command(args.samples)
    if args.settle + args.samples / args.rate + 2 >= args.seconds:
        raise ValueError("timed TX must exceed settle + capture duration + 2 s margin")
    if args.lo != args.mhz * 1000000:
        raise ValueError("bench analysis requires RX LO equal to the requested board center")
    if args.mode == "dvb" and (args.rate < 2000000 or args.samples / args.rate < .6):
        raise ValueError("DVB needs >=2 MS/s and >=0.6 s to compare a complete eight-frame loop")
    if args.mode == "tx" and args.rate < (8000000 if args.signal == "two" else 4000000):
        raise ValueError("tone capture needs >=4 MS/s; two tones need >=8 MS/s")
    # MHz: 1 Msym/s * (1 + 0.35) / 2 + 0.125 MHz carrier-search margin.
    # Firmware rate/rolloff: dvbtx.c:9. Tones use capture.py's +/-100 kHz search.
    edge = .8 if args.mode == "dvb" else (3.1 if args.signal == "two" else 1.1)
    if args.mhz - edge < 2390 or args.mhz + edge > 2450:
        raise ValueError("signal plus carrier-offset margin must fit inside 2390..2450 MHz")
    command = (f"dvb {args.mhz} {args.modcod} {args.seconds} {args.gain} {args.peak}" if args.mode == "dvb"
               else f"tx {args.mhz} {args.signal} {args.seconds} {args.gain}")
    return {"command": command, "rf_path": path, "sdr_requested": asdict(radio.settings),
            "configure": [radio.attr_command(a, a[3]) for a in radio.settings.attributes()],
            "readback": [radio.attr_command(a) for a in radio.settings.attributes()],
            "board_info": board_command("info", args), "board": board_command(command, args),
            "settle_seconds": args.settle, "capture": radio.read_command(args.samples),
            "capture_seconds": args.samples / args.rate,
            "analysis": analysis_command(args, pathlib.Path("<run-id>.iq"), pathlib.Path("<run-id>-receiver.log")),
            "timing_assumption": "settle starts at board.py launch; firmware has no RF-start acknowledgement"}


def append(path: pathlib.Path, record: dict):
    with path.open("a") as f:
        f.write(json.dumps(record, allow_nan=False) + "\n")
        f.flush()


def board_records(stdout: str) -> list[dict]:
    records = [json.loads(line) for line in stdout.splitlines() if line.strip()]
    if not records or any(not isinstance(r, dict) for r in records):
        raise ValueError("board.py returned no JSON records")
    return records


def analysis_command(args, capture: pathlib.Path, log: pathlib.Path) -> list[str]:
    if args.mode == "tx":
        cmd = [sys.executable, str(ROOT / "host/capture.py"), str(capture), "--format", "i16",
               "--rate", str(args.rate), "--scale", "2048", "--signal", "two" if args.signal == "two" else "tone"]
    else:
        cmd = [sys.executable, str(ROOT / "host/onair.py"), str(capture), "--rate", str(args.rate),
               "--scale", "2048", "--modcod", str(args.modcod), "--auto-shift", "--mer", "--log", str(log)]
        if args.receiver:
            cmd += ["--receiver", str(args.receiver)]
    return cmd


def analyze(args, capture: pathlib.Path, log: pathlib.Path) -> tuple[dict, bool]:
    cmd = analysis_command(args, capture, log)
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=240, check=False)
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        report = {"error": "analysis did not return JSON", "stdout": result.stdout}
    report.update({"analysis_command": cmd, "returncode": result.returncode, "stderr": result.stderr})
    return report, result.returncode == 0 and "error" not in report


def execute(args, radio: sdr.SDR, plan: dict, directory: pathlib.Path) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    readme = directory / "README.md"
    if not readme.exists():
        with readme.open("x") as f:
            f.write("# Cabled bench runs\n\nOperator RF paths, board records and run outcomes are appended to runs.jsonl.\n"
                    "Capture read-backs, hashes and analysis are appended to captures.jsonl.\n"
                    "Raw .iq files and sidecars are retained locally. Failed attempts are retained.\n"
                    "Gain indices and ADC counts are not calibrated power measurements.\n")
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]
    cap = directory / (run_id + ".iq")
    runs = directory / "runs.jsonl"
    captures = directory / "captures.jsonl"
    base = {"run_id": run_id, "host_time": sdr.now()}
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                              capture_output=True, check=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, text=True,
                           capture_output=True, check=True).stdout.strip()
    hashes = {name: hashlib.sha256((ROOT / "host" / name).read_bytes()).hexdigest()
              for name in ("bench.py", "sdr.py", "onair.py", "capture.py", "board.py",
                           "decode.py", "iq.py", "dvbs2.py", "dvbs2_tables.py")}
    append(runs, {**base, "event": "start", "plan": plan, "host_revision": revision,
                  "host_dirty": bool(dirty), "host_tools_sha256": hashes})
    outcome = {**base, "event": "finish", "status": "failed", "capture": str(cap)}
    process = None
    try:
        if args.mode == "dvb":
            receiver = args.receiver or os.environ.get("LEANDVB", ROOT / "notes/leansdr/src/apps/leandvb")
            executable = shutil.which(str(receiver))
            if not executable:
                raise FileNotFoundError(f"LeanDVB not found: {receiver}; pass --receiver before transmitting")
            with open(executable, "rb") as f:
                outcome["receiver_sha256"] = hashlib.file_digest(f, "sha256").hexdigest()
        configured = radio.configure()
        append(runs, {**base, "event": "sdr_configured", "metadata": configured})
        info = subprocess.run(plan["board_info"], text=True, capture_output=True, timeout=30, check=False)
        records = board_records(info.stdout)
        for r in records:
            append(runs, {**base, "event": "board", "command": "info", "record": r})
        if (info.returncode or not any(r.get("cmd") == "info" and r.get("firmware") for r in records)
                or any("error" in r for r in records)):
            raise RuntimeError(f"board info failed: {info.stderr}")
        # Separate process group: Ctrl-C must not discard the finite TX's records.
        process = subprocess.Popen(plan["board"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        started = time.monotonic()
        outcome["board_started_at"] = sdr.now()
        time.sleep(args.settle)
        if process.poll() is not None:
            raise RuntimeError("board command ended before capture")

        def ready():
            if process.poll() is not None:
                raise RuntimeError("board command ended during SDR verification")
            if time.monotonic() - started + args.samples / args.rate + 1 >= args.seconds:
                raise RuntimeError("not enough timed TX remaining after SDR verification")

        metadata = radio.capture(cap, args.samples, before_capture=ready)
        append(captures, {**base, "event": "capture", "metadata": metadata})
        report, passed = analyze(args, cap, directory / (run_id + "-receiver.log"))
        append(captures, {**base, "event": "analysis", "capture": str(cap), "analysis": report})
        outcome["analysis_pass"] = passed
        if not passed:
            raise RuntimeError("capture analysis failed; see captures.jsonl")
        outcome["status"] = "ok"
    except (Exception, KeyboardInterrupt) as exc:
        outcome["error"] = f"{type(exc).__name__}: {exc}"
        sidecar = cap.with_suffix(".iq.json")
        if sidecar.exists() and not outcome.get("analysis_pass"):
            append(captures, {**base, "event": "capture_state", "metadata": json.loads(sidecar.read_text())})
    finally:
        if process is not None:
            try:
                stdout, stderr = process.communicate(timeout=args.seconds + 30)
                # Keep the raw stream too, including partial/error output.
                (directory / (run_id + "-board.log")).write_text(stdout + "\nSTDERR:\n" + stderr)
                records = board_records(stdout)
                for r in records:
                    append(runs, {**base, "event": "board", "command": plan["command"], "record": r})
                good = (not process.returncode and any(r.get("cmd") == args.mode for r in records)
                        and not any("error" in r or r.get("fatal_late", 0) or r.get("late_fills", 0)
                                    or r.get("status", "ESP_OK") != "ESP_OK" for r in records))
                outcome["board_completed"] = good
                if not good:
                    raise RuntimeError(f"board command failed: exit {process.returncode}; {stderr}")
            except (Exception, KeyboardInterrupt) as exc:
                outcome.update({"status": "failed", "board_error": str(exc), "board_completed": False})
                if process.poll() is None:
                    process.kill()
                    process.communicate()
                    outcome["rf_stop_unconfirmed"] = True
        outcome["finished_at"] = sdr.now()
        append(runs, outcome)
    return outcome


def main() -> int:
    args = parser().parse_args()
    try:
        path = rf_path(args)
        radio = sdr.from_args(args)
        plan = make_plan(args, radio, path)
        directory = ROOT / "results" / (dt.date.today().isoformat() + "-" + args.topic)
        if args.dry_run:
            print(json.dumps({"dry_run": True, "results": str(directory), **plan}, indent=2))
            for cmd in plan["configure"] + plan["readback"] + [plan["board_info"], plan["board"], plan["capture"], plan["analysis"]]:
                print(shlex.join(cmd))
            print("Verify versions/scan format, repeat read-backs before capture; append runs.jsonl and captures.jsonl.")
            return 0
        (ROOT / "notes").mkdir(exist_ok=True)
        with (ROOT / "notes/bench.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            outcome = execute(args, radio, plan, directory)
        print(json.dumps(outcome))
        return 0 if outcome["status"] == "ok" else 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(json.dumps({"error": str(exc)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
