"""Bench orchestration with executable IIO/board fakes. No serial/network access."""
import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from unittest.mock import patch

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import bench
import sdr
from test_sdr import calls, fake_tools, rejected


BOARD = r'''
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ["FAKE_IIO"])
command = sys.argv[1]
with (root / "board-calls.jsonl").open("a") as f:
    f.write(json.dumps(command) + "\n")
assert json.loads((root / "state.json").read_text())["o/altvoltage1/powerdown"] == "1"
if command == "info":
    r = {"cmd": "info", "firmware": "fake-version", "idf": "fake-idf"}
    if (root / "callsign").exists():
        r["callsign"] = (root / "callsign").read_text()
    print(json.dumps(r))
elif (root / "board_early").exists():
    print(json.dumps({"cmd": command.split()[0], "error": "fake refused"}))
    sys.exit(1)
else:
    time.sleep(2)
    r = {"cmd": command.split()[0], "status": "ESP_OK", "late_fills": 0, "fatal_late": 0}
    if (root / "board_error").exists():
        r["error"] = "fake radio failure"
    print(json.dumps(r))
'''


def arguments(prefix, mode="tx"):
    return bench.parser().parse_args([mode, "--topic", "fake-bench", "--attenuation", "20", "10",
                                     "--cable", "U.FL-SMA + test coax + RX1", "--radiates", "no",
                                     "--authorize-tx", "--libiio", str(prefix), "--seconds", "10", "--settle", ".01",
                                     "--samples", "32768"])


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def test_run():
    with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"FAKE_IIO": tmp}):
        root = pathlib.Path(tmp)
        prefix = fake_tools(root)
        board = root / "board.py"
        board.write_text(BOARD)
        args = arguments(prefix)
        z = 500 * np.exp(2j * np.pi * 1055000 * np.arange(args.samples) / args.rate)
        np.rint(np.column_stack([z.real, z.imag])).astype("<i2").tofile(root / "samples.iq")
        with patch.object(bench, "board_command", lambda command, args: [sys.executable, str(board), command]):
            radio = sdr.from_args(args)
            plan = bench.make_plan(args, radio, bench.rf_path(args))
            out = root / "results"
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "ok" and result["board_completed"], result
            run = records(out / "runs.jsonl")
            cap = records(out / "captures.jsonl")
            assert run[0]["plan"]["rf_path"]["total_attenuation_db"] == 30
            assert not run[0]["plan"]["rf_path"]["radiates"]
            assert run[0]["host_tools_sha256"]["sdr.py"]
            assert run[2]["record"]["firmware"] == "fake-version"
            analysis = next(r["analysis"] for r in cap if r["event"] == "analysis")
            assert abs(analysis["carrier_offset_hz"] - 55000) < 5, analysis
            assert "2048" in analysis["analysis_command"]
            assert abs(analysis["tones"][0]["power_dbfs"] - 20 * np.log10(500 / 2048)) < .05
            assert [r["record"]["cmd"] for r in run if r["event"] == "board"] == ["info", "tx"]
            previous = (out / "runs.jsonl").read_bytes()
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "ok" and (out / "runs.jsonl").read_bytes().startswith(previous)
            # Failure after keying still waits for and records the timed board result.
            (root / "short").touch()
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "failed" and result["board_completed"], result
            assert any(r["event"] == "capture_state" and r["metadata"]["status"] == "failed"
                       for r in records(out / "captures.jsonl"))
            (root / "short").unlink()
            (root / "board_error").touch()
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "failed" and not result["board_completed"], result
            (root / "board_error").unlink()
            (root / "board_early").touch()
            args.settle = .1
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "failed" and "ended before capture" in result["error"], result
            (root / "board_early").unlink()
            # New firmware refuses an empty callsign before the keyed subprocess.
            (root / "callsign").write_text("")
            before = records(root / "board-calls.jsonl")
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "failed" and "no callsign" in result["error"]
            assert records(root / "board-calls.jsonl")[len(before):] == ["info"]
            (root / "callsign").write_text("N0CALL")
            args.seconds = 20
            # Verify the computed wait and timeout without waiting for CW in this
            # executable fake. The board fake's own 2 s run still completes.
            real_sleep = bench.time.sleep
            waits = []
            with patch.object(bench.time, "sleep", lambda seconds: (waits.append(seconds), real_sleep(.1))[-1]):
                result = bench.execute(args, radio, plan, out)
            assert result["status"] == "ok", result
            assert abs(waits[0] - (args.settle + bench.morse.duration("N0CALL"))) < 1e-9
            args.mode = "dvb"
            assert bench.analysis_command(args, pathlib.Path("x"), pathlib.Path("y"))[-2:] == ["--callsign", "N0CALL"]
            args.mode = "tx"
            (root / "callsign").unlink()
            args.seconds = 10
            # Configuration read-back mismatch prevents even the info command.
            before = (root / "board-calls.jsonl").read_bytes()
            (root / "override.json").write_text(json.dumps({"i/voltage0/sampling_frequency": "1"}))
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "failed" and "read-back" in result["error"]
            assert (root / "board-calls.jsonl").read_bytes() == before
            args.mode, args.samples, args.receiver = "dvb", 4000000, root / "absent-leandvb"
            plan = bench.make_plan(args, radio, bench.rf_path(args))
            before_iio = calls(root)
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "failed" and "LeanDVB not found" in result["error"]
            assert calls(root) == before_iio
            assert (root / "board-calls.jsonl").read_bytes() == before
    print("bench: executable fakes, real tone analysis, ordering, RF bookkeeping, append-only records, failed capture/board/read-back cleanup")


def test_validation_and_dry_run():
    args = arguments(pathlib.Path("/no-tools"))
    for field, bad in (("attenuation", None), ("cable", ""), ("radiates", None),
                       ("attenuation", [20]), ("attenuation", [float("nan")]),
                       ("attenuation", [40, -1]), ("authorize_tx", False)):
        with patch.object(args, field, bad), patch("sys.stdin.isatty", return_value=False):
            rejected(lambda: bench.rf_path(args))
    for field, bad in (("topic", "../escape"), ("seconds", 1), ("settle", float("nan")),
                       ("mhz", 100), ("lo", 2400000000), ("samples", 0)):
        with patch.object(args, field, bad):
            rejected(lambda: bench.make_plan(args, sdr.from_args(args), bench.rf_path(args)))
    with patch("sys.argv", ["bench.py", "dvb", "--topic", "fake-dry", "--attenuation", "30",
                            "--cable", "test", "--radiates", "no", "--dry-run"]), \
            patch.object(bench.subprocess, "run", side_effect=AssertionError("dry run executed a command")), \
            patch.object(bench.subprocess, "Popen", side_effect=AssertionError("dry run started a process")), \
            contextlib.redirect_stdout(io.StringIO()) as output:
        assert bench.main() == 0
        assert '"dry_run": true' in output.getvalue() and "dvb 2420 4 30 0 128" in output.getvalue()
    args.mode = "dvb"
    args.samples = 4000000
    with patch.object(bench.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, '{"payload_match":true}', "")) as run:
        report, ok = bench.analyze(args, pathlib.Path("fake.iq"), pathlib.Path("fake.log"))
        assert ok
        cmd = run.call_args.args[0]
        assert "--auto-shift" in cmd and "--mer" in cmd and "onair.py" in cmd[1]
    print("bench: missing/unsafe RF path and authorization refused, invalid plans, zero-I/O dry run, automatic DVB carrier/MER command")


def test_dvb_run():
    import dvbs2
    import iq
    import onair

    with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"FAKE_IIO": tmp}):
        root = pathlib.Path(tmp)
        prefix = fake_tools(root)
        board = root / "board.py"
        board.write_text(BOARD)
        receiver = root / "leandvb"
        # The fake FEC process checks that the automatic frequency correction
        # actually reaches its input, then emits known BBFRAMEs/status records.
        receiver.write_text(f"#!{sys.executable}\n" + r'''
import os, pathlib, sys
import numpy as np
root = pathlib.Path(os.environ["FAKE_IIO"])
z = np.frombuffer(sys.stdin.buffer.read(), dtype="<c8")[:262144]
p = abs(np.fft.fft(z**4 * np.hanning(len(z))))
assert abs(np.fft.fftfreq(len(z), 1/4000000)[np.argmax(p)] / 4) < 50
fd = int(sys.argv[sys.argv.index("--fd-bbf") + 1])
os.write(fd, (root / "bbframes.bin").read_bytes())
print("FRAMELOCK 1", file=sys.stderr)
for _ in range(8):
    print("S2FEC modcod=4 short=0 pilots=1 ldpc_flips=0 ldpc_checks_before=0 ldpc_checks_after=0 bch_errors=0", file=sys.stderr)
''')
        receiver.chmod(0o755)
        (root / "callsign").write_text("N0CALL")
        data = onair.payload(4, callsign="N0CALL")
        per = len(data) // 8
        header = np.packbits(dvbs2.bbheader(dvbs2.matype1(dvbs2.GCS, .35), per * 8)).tobytes()
        (root / "bbframes.bin").write_bytes(b"".join(header + data[i:i + per] for i in range(0, len(data), per)))
        args = arguments(prefix, "dvb")
        args.samples, args.receiver, args.seconds = 2400000, receiver, 14
        # The firmware's continuous stream: frames 0..23, no repetition.
        phases = (np.rint(np.angle(onair.symbols(4, 24, "N0CALL")) * 4 / np.pi).astype(int) % 8).astype(np.uint8)
        z = iq.shape(phases, 1e6, 4e6, frequency_offset=55000,
                     sample_rate_offset_ppm=70, snr_db=20 - 10 * np.log10(4), seed=9)[123:123 + args.samples]
        np.rint(np.column_stack([z.real, z.imag]) * 200).astype("<i2").tofile(root / "samples.iq")
        with patch.object(bench, "board_command", lambda command, args: [sys.executable, str(board), command]), \
                patch.object(bench.time, "sleep", lambda seconds: None):
            radio = sdr.from_args(args)
            plan = bench.make_plan(args, radio, bench.rf_path(args))
            out = root / "results"
            result = bench.execute(args, radio, plan, out)
            assert result["status"] == "ok" and result["board_completed"], result
            report = next(r["analysis"] for r in records(out / "captures.jsonl") if r["event"] == "analysis")
            assert report["payload_match"] and report["frame_lock"] and report["callsign"] == "N0CALL", report
            assert abs(report["mer"]["mer_db"] - 20) < .3, report
            assert abs(report["shift_hz"] - 55000 / 1.000070) < 1, report
            assert result["receiver_sha256"]
    print("bench DVB: synthetic QPSK through executable device/FEC fakes, real carrier/timing/MER, corrected receiver input, payload bookkeeping")


if __name__ == "__main__":
    test_validation_and_dry_run()
    test_run()
    test_dvb_run()
