#!/usr/bin/env python3
"""Coordinator-only E200 throughput probe or contained USB-S3/live TS proof.

--plan never opens devices. Probe never opens the S3. Keyed runs require both
--key and --closed-box; no flashing, firmware changes, or console setup here.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from check_ts import check
from iio_stream import write_json
from make_ts import make_ts
from receive import stop

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RATES = (10_000_000, 15_000_000, 20_000_000)


def worker_command(a, rate, report):
    return [a.iio_python, str(HERE / "iio_stream.py"), "--probe", "--uri", a.uri,
            "--freq", str(a.mhz*1e6), "--gain", str(a.rx_gain), "--rate", str(rate),
            "--symbol-rate", str(5e6 if rate == 10e6 else 10e6),
            "--seconds", str(a.probe_seconds), "--report", str(report)]


def eligible(probe, a, rate):
    if (probe.get("uri") != a.uri or probe.get("frequency") != a.mhz*1e6 or
            probe.get("rx_gain") != a.rx_gain):
        return False
    for row in probe.get("rates", []):
        if (row.get("requested_rate") == rate and row.get("complete") is True and
                row.get("host_timing_passed") is True and row.get("exit") == 0 and
                row.get("requested_seconds", 0) >= 29 and row.get("sample_seconds", 0) >= 29):
            return True
    return False


def choose_rate(probe, a):
    candidates = (10_000_000,) if a.modcod == 13 else (20_000_000, 15_000_000)
    for rate in candidates:
        if eligible(probe, a, rate):
            return rate
    raise ValueError("no eligible 30 s probe at this URI/frequency/gain for requested mode; S3 was not opened")


def keyed_commands(a, rate):
    out = a.out.resolve()
    rs = 5_000_000 if a.modcod == 13 else 10_000_000
    rx = [sys.executable, str(HERE / "receive.py"), "--backend", "libiio", "--uri", a.uri,
          "--iio-python", a.iio_python, "--freq", str(a.mhz*1e6), "--gain", str(a.rx_gain),
          "--rate", str(rate), "--symbol-rate", str(rs), "--modcod", str(a.modcod),
          "--upsample", "1", "--seconds", str(a.seconds+25),
          "--receiver", a.receiver, "--output", str(out / "received.ts"),
          "--capture-report", str(out / "capture.json"), "--ready", str(out / "ready.json"),
          "--stop-file", str(out / "stop")]
    if a.player:
        rx.extend(("--player", a.player))
    tx = [sys.executable, str(ROOT / "examples/usb-send/send.py"), "send", str(out / "source.ts"),
          "--kind", "ts", "--port", a.port, "--key", "--frequency", str(a.mhz),
          "--gain", str(a.tx_gain), "--modcod", str(a.modcod), "--rate", "5m" if a.modcod == 13 else "10m",
          "--seconds", str(a.seconds), "--report", str(out / "sender.json")]
    return rx, tx


def wait_ready(process, out, timeout=30):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("receiver failed before readiness; S3 was not opened; see receiver.log")
        ready = out / "ready.json"
        if ready.exists() and json.loads(ready.read_text()).get("ready") is True:
            return
        time.sleep(.05)
    raise RuntimeError("receiver readiness timed out; S3 was not opened")


def verify(out, a):
    sender = json.loads((out / "sender.json").read_text())
    capture = json.loads((out / "capture.json").read_text())
    stats = sender["sender"]
    if (sender["errors"] or stats["errors"] or not stats["source_eof"] or
            not stats["complete"] or not stats["terminal_nop"] or stats["write_uncertain"]):
        raise ValueError("sender did not finish the finite TS cleanly; see sender.json")
    # Reuse the sender's strict USB/encoder/deadline/counter audit offline.
    sys.path.insert(0, str(ROOT / "examples/usb-send"))
    from send import check_run
    check_run(stats, sender["records"], require_eof=True)
    runs = [r for r in sender["records"] if "usb" in r]
    row = runs[0]
    expected = dict(cmd="dvb", modcod=a.modcod, symbol_rate=5_000_000 if a.modcod == 13 else 10_000_000,
                    freq_mhz=a.mhz, seconds=a.seconds, gain=a.tx_gain, payload_source="usb", input="ts")
    if any(row.get(k) != v for k, v in expected.items()) or not row.get("callsign"):
        raise ValueError("board RF mode/source/callsign differs; see sender.json")
    if not capture["complete"] or not capture.get("host_timing_passed"):
        raise ValueError("RX incomplete or host timing suspects sample loss")
    source, received = (out / "source.ts").read_bytes(), (out / "received.ts").read_bytes()
    if stats["payload_bytes"] != len(source) or row["usb"]["payload_bytes"] != len(source):
        raise ValueError("source bytes differ from sent/accepted bytes")
    result = check(received, source)
    if len(received) < .90*len(source):
        raise ValueError("less than 90% of sent TS recovered")
    result.update(passed=True, received_fraction=len(received)/len(source),
                  sent_bytes=len(source), received_bytes=len(received),
                  source_sha256=hashlib.sha256(source).hexdigest(),
                  received_sha256=hashlib.sha256(received).hexdigest(),
                  scope="exact contiguous sent TS slice; acquisition/EOF trims allowed; no interior loss",
                  sample_continuity="host symptoms checked; TS payload proves continuity only across recovered slice")
    return result


def probe(a):
    result = dict(passed=False, uri=a.uri, frequency=a.mhz*1e6, rx_gain=a.rx_gain, rates=[],
                  scope="receive-only discard throughput; no S3, no decoder; hardware sample continuity unknown")
    for rate in RATES:
        path = a.out / f"{rate//1000000}m.json"
        cmd = worker_command(a, rate, path)
        with (a.out / f"{rate//1000000}m.log").open("w") as log:
            process = None
            try:
                process = subprocess.Popen(cmd, stdout=log, stderr=log)
                code = process.wait(timeout=a.probe_seconds+25)
                row = json.loads(path.read_text()) if path.exists() else dict(complete=False, error="worker made no report")
                row["exit"] = code
            except subprocess.TimeoutExpired:
                row = dict(complete=False, error="probe deadline", exit=1)
            finally:
                stop(process, timeout=15)
        row.update(requested_rate=rate, command=cmd)
        result["rates"].append(row)
        write_json(a.out / "report.json", result)
        print(f"{rate/1e6:g} MS/s: {'eligible' if eligible(result, a, rate) else 'failed'}", flush=True)
    result["passed"] = eligible(result, a, 10_000_000)
    result["qpsk_rates"] = [rate for rate in RATES[1:] if eligible(result, a, rate)]
    write_json(a.out / "report.json", result)
    return 0 if result["passed"] else 1


def keyed(a, rate):
    rx, tx = keyed_commands(a, rate)
    result = dict(passed=False, mode=a.modcod, rate=rate, receiver_command=rx, sender_command=tx)
    receiver = sender = None
    try:
        source = a.ts.read_bytes() if a.ts else make_ts(200)
        check(source)
        (a.out / "source.ts").write_bytes(source)
        with (a.out / "receiver.log").open("w") as rxlog, (a.out / "sender.log").open("w") as txlog:
            receiver = subprocess.Popen(rx, stdout=rxlog, stderr=rxlog)
            wait_ready(receiver, a.out)
            sender = subprocess.Popen(tx, stdout=txlog, stderr=txlog)
            deadline = time.monotonic()+a.seconds+35
            rx_failed = False
            while sender.poll() is None:
                # A timed S3 session retains serial ownership until done/NOP, even
                # if RX fails. It never retries a transmission or uncertain write.
                rx_failed |= receiver.poll() is not None
                if time.monotonic() > deadline:
                    raise RuntimeError("sender deadline exceeded; firmware retains its timed RF window")
                time.sleep(.05)
            time.sleep(1)
            (a.out / "stop").touch()
            rx_code = receiver.wait(timeout=20)
            if sender.returncode or rx_code or rx_failed:
                raise RuntimeError(f"live run failed: sender={sender.returncode}, receiver={rx_code}; see logs")
        result.update(verify(a.out, a))
    except (Exception, KeyboardInterrupt) as exc:
        result.update(passed=False, error=str(exc) or "interrupted; firmware retains timed RF window")
    finally:
        (a.out / "stop").touch()
        stop(sender)
        stop(receiver, timeout=20)
        write_json(a.out / "report.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=("probe", "keyed"))
    p.add_argument("out", type=Path, help="new evidence directory")
    p.add_argument("--uri", required=True)
    p.add_argument("--mhz", type=int, required=True, help="assigned integer MHz")
    p.add_argument("--iio-python", default=sys.executable)
    p.add_argument("--rx-gain", type=float, default=0)
    p.add_argument("--probe-seconds", type=float, default=30)
    p.add_argument("--probe-report", type=Path, help="required for keyed runs; gates rate selection")
    p.add_argument("--receiver", default=os.environ.get("RX", "leandvb"))
    p.add_argument("--port", help="explicit S3 native USB serial port")
    p.add_argument("--key", action="store_true")
    p.add_argument("--closed-box", action="store_true")
    p.add_argument("--modcod", type=int, choices=(13, 7), default=13)
    p.add_argument("--seconds", type=int, default=60, help="keyed window including both IDs")
    p.add_argument("--tx-gain", type=int, default=0)
    p.add_argument("--ts", type=Path, help="finite TS; default 8 s of 9 Mb/s test video (~23 s of USB intake)")
    p.add_argument("--player", help="optional receive.py live player command")
    p.add_argument("--plan", action="store_true")
    return p


def main():
    p = parser()
    a = p.parse_args()
    try:
        if (not a.uri.startswith(("ip:", "usb:")) or not a.uri.split(":", 1)[1].strip() or
                not 2390 <= a.mhz <= 2450 or not math.isfinite(a.rx_gain) or not 0 <= a.rx_gain <= 76 or
                not math.isfinite(a.probe_seconds) or not 29 <= a.probe_seconds <= 120 or
                not 1 <= a.seconds <= 60 or not 0 <= a.tx_gain <= 17):
            raise ValueError("invalid URI, frequency, gain or duration")
        if a.action == "probe":
            if a.key:
                raise ValueError("probe is receive-only; --key is invalid")
            plan = dict(commands=[worker_command(a, rate, a.out / f"{rate//1000000}m.json") for rate in RATES])
        else:
            if not a.plan and (not a.key or not a.closed_box or not a.port or not a.probe_report):
                raise ValueError("keyed run requires --key --closed-box --port --probe-report")
            rate = choose_rate(json.loads(a.probe_report.read_text()), a) if a.probe_report else (10_000_000 if a.modcod == 13 else 20_000_000)
            rx, tx = keyed_commands(a, rate)
            plan = dict(receiver=rx, sender=tx)
        if a.plan:
            print(json.dumps(plan, indent=2))
            return 0
        if not shutil.which(a.iio_python) or (a.action == "keyed" and not shutil.which(a.receiver)):
            raise ValueError("capture Python or LeanDVB executable missing")
        a.out = a.out.resolve()
        a.out.mkdir(parents=True, exist_ok=False)
        write_json(a.out / "plan.json", plan)
        write_json(a.out / "revision.json", dict(revision=subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True))))
        return probe(a) if a.action == "probe" else keyed(a, rate)
    except (OSError, ValueError) as exc:
        p.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
