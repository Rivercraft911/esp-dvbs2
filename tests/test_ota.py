"""OTA guards, UHD failure handling and offline receiver boundary; no devices."""
import contextlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples/over-the-air"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ota = load("ota", EXAMPLE / "check.py")
capture = load("ota_capture", EXAMPLE / "capture.py")
import decode


def rejects(fn, *args):
    try:
        fn(*args)
    except (ValueError, RuntimeError):
        return
    raise AssertionError("invalid evidence accepted")


def test_guards():
    for rate, fs, occupied in (("5m", 10e6, 6.75e6), ("10m", 20e6, 13.5e6)):
        for mc in (7, 13, 11):
            output = subprocess.check_output([sys.executable, "-S", str(EXAMPLE / "check.py"),
                                              "--plan", "--rate", rate, "--modcod", str(mc)])
            plan = json.loads(output)
            assert plan["rf"]["sample_rate"] == fs and plan["rf"]["occupied_hz"] == occupied
            assert plan["rf"]["master_clock_rate"] / fs in (2, 4)
            assert plan["rf"]["bandwidth"] <= 20e6
            assert plan["board_command"] == f"dvb 2420 {mc} {rate} 30 0 128"
    for extra in ([], ["--args", "addr=fake"], ["--key"],
                  ["--dry-run", "missing", "--callsign", "N0CALL", "--dump-rate", "20e6", "--key"],
                  ["--plan", "--args", "addr=fake,master_clock_rate=61440000"],
                  ["--plan", "--args", "type=b200"], ["--plan", "--rate", "20m"],
                  ["--plan", "--rx-gain", "nan"]):
        result = subprocess.run([sys.executable, "-S", str(EXAMPLE / "check.py"), *extra],
                                capture_output=True)
        assert result.returncode == 2 and b"ModuleNotFoundError" not in result.stderr
    result = subprocess.run([sys.executable, "-S", str(EXAMPLE / "capture.py"), "missing"],
                            capture_output=True)
    assert result.returncode == 2 and b"requires explicit --key" in result.stderr
    for call in ("", "BAD", "N0/CALL/", "☃"):
        rejects(ota.stored_callsign, [{"callsign": call}])
    rejects(ota.stored_callsign, [{"callsign": "N0CALL"}], "K1ABC")
    rejects(ota.stored_callsign, [{"callsign": "N0CALL"}], None, 10)
    assert ota.stored_callsign([{"callsign": "N0CALL"}]) == "N0CALL"


def test_readback_and_metrics():
    args = ota.parser().parse_args([])
    s = ota.settings(args)
    actual = {k: s[k] for k in ("sample_rate", "master_clock_rate", "frequency", "bandwidth", "gain", "antenna")}
    actual["lo_locked"] = True
    capture.check_readback(actual, s)
    for key, value in (("sample_rate", 9.9e6), ("master_clock_rate", 61.44e6),
                       ("bandwidth", 56e6), ("bandwidth", 5e6), ("frequency", float("nan")),
                       ("gain", 20), ("antenna", "wrong"), ("lo_locked", False)):
        rejects(capture.check_readback, dict(actual, **{key: value}), s)
    r = decode.summarize("MER -99\nFREQ 99999\nFRAMELOCK 1\nMER 12.0\nFREQ -25000\n"
                         "MER 14\nFREQ -24000\nFRAMELOCK 0\nMER -90\nFREQ 0\n", [], b"")
    assert r["mer_db"]["median"] == 13 and r["frequency_offset_hz"]["median"] == -24500
    assert decode.summarize("", [], b"")["mer_db"] is None


def test_coordinator():
    args = ota.parser().parse_args(["--key", "--args", "addr=fake"])
    s = ota.settings(args)
    sent = []
    info = [{"cmd": "info", "callsign": "N0CALL"}]
    row = dict(cmd="dvb", callsign="N0CALL", status="ESP_OK", modcod=7, symbol_rate=5000000,
               payload_source="xorshift32", encoder_failed=False, underruns=0,
               frames_played=15, frames_encoded=18, freq_mhz=2420, seconds=30, gain=0, peak_level=128)
    def run(port, text, **kw):
        assert not kw["reset"]
        sent.append(text)
        return info if text == "info" else [row] if text.startswith("dvb ") else [{"cmd": "spiinput"}]
    board = types.SimpleNamespace(run=run, reserve_port=lambda p: contextlib.nullcontext())
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        class Process:
            def wait(self, **kw):
                assert (out / "stop").exists()
                (out / "capture.json").write_text(json.dumps(dict(complete=True, overflows=0)))
                return 0
            def poll(self):
                return 0
        def ready(process, folder, settings):
            assert sent == ["info", "spiinput off"]
        with patch.dict(sys.modules, board=board), patch.object(ota.subprocess, "Popen", return_value=Process()) as start, \
                patch.object(ota, "wait_ready", side_effect=ready), patch.object(ota.time, "sleep"):
            assert ota.hardware(args, s, out)[:2] == ("N0CALL", 18)
            assert "--key" in start.call_args[0][0]
            sent.clear()
            info[0]["callsign"] = ""
            rejects(ota.hardware, args, s, out)
            assert sent == ["info"] and start.call_count == 1
            info[0]["callsign"] = "N0CALL"
            sent.clear()
            with patch.object(ota, "wait_ready", side_effect=RuntimeError("no IQ")):
                rejects(ota.hardware, args, s, out)
            assert sent == ["info", "spiinput off"]
    for key, value in (("status", "ESP_FAIL"), ("payload_source", "usb"), ("modcod", 11),
                       ("underruns", 1), ("encoder_failed", True), ("fatal_late", 1)):
        rejects(ota.board_result, [dict(row, **{key: value})], args, "N0CALL")

    solo = ota.parser().parse_args(["--modcod", "17", "--rate", "10m", "--phy-solo", "--plan"])
    assert ota.tx_command(solo).endswith(" --phy-solo")
    # Solo telemetry must prove teardown before playback setup and clean release.
    solo_row = dict(row, modcod=17, symbol_rate=10000000, phy_solo=True,
                    wifi_deinit_at_us=2, session=dict(at_us=3, wifi_initialized=False,
                    wifi_mode_result=0x3001, phy_users=2, wifi_irq_routes=[0,0], irq_sampled_cores=3),
                    after=dict(wifi_initialized=False, phy_users=0))
    assert ota.board_result([solo_row], solo, "N0CALL") == row["frames_encoded"]
    rejects(ota.board_result, [dict(solo_row, session={})], solo, "N0CALL")


def test_capture_worker():
    # Exercise the real capture loop against a fake UHD API, including receive
    # metadata errors and a coerced clock before readiness. Never load UHD.
    s = ota.settings(ota.parser().parse_args(["--args", "addr=fake"]))
    for fault in (None, "overflow", "timeout", "coercion"):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            calls = []
            class Stream:
                received = 0
                def issue_stream_cmd(self, command):
                    calls.append(command.mode)
                def recv(self, buffer, metadata, timeout):
                    self.received += 1
                    buffer[:] = .25j
                    metadata.time_spec = types.SimpleNamespace(get_real_secs=lambda: 12.5)
                    metadata.error_code = fault if fault and self.received == 2 else "none"
                    metadata.strerror = lambda: "injected " + str(fault)
                    if fault is None:
                        (out / "stop").touch()
                    return 4
            stream = Stream()
            class Device:
                def __getattr__(self, name):
                    if name.startswith("set_"):
                        return lambda *args: calls.append((name, args))
                    values = dict(get_master_clock_rate=39e6 if fault == "coercion" else 40e6,
                                  get_rx_rate=10e6, get_rx_bandwidth=10e6,
                                  get_rx_freq=2420e6, get_rx_gain=0, get_rx_antenna="RX2")
                    return lambda *args: values[name]
                def get_rx_sensor(self, *args):
                    return types.SimpleNamespace(to_bool=lambda: True)
                def get_usrp_rx_info(self, *args):
                    return {"name": "fake"}
                def get_rx_stream(self, args):
                    assert args.channels == [0]
                    return stream
            uhd = types.SimpleNamespace(
                usrp=types.SimpleNamespace(MultiUSRP=lambda args: Device(),
                                          StreamArgs=lambda *args: types.SimpleNamespace()),
                types=types.SimpleNamespace(TuneRequest=lambda freq: freq,
                    RXMetadata=types.SimpleNamespace,
                    RXMetadataErrorCode=types.SimpleNamespace(none="none", overflow="overflow"),
                    StreamCMD=lambda mode: types.SimpleNamespace(mode=mode),
                    StreamMode=types.SimpleNamespace(start_cont="start", stop_cont="stop")),
                get_version_string=lambda: "fake UHD")
            with patch.dict(sys.modules, uhd=uhd):
                assert capture.capture(s, out) == (0 if fault is None else 1)
            report = json.loads((out / "capture.json").read_text())
            assert report["complete"] == (fault is None)
            assert report["overflows"] == int(fault == "overflow")
            if fault == "coercion":
                assert not (out / "ready.json").exists() and "start" not in calls
            else:
                assert report["samples"] == 4 and calls[-1] == "stop"
                assert (out / "capture.cfile").stat().st_size == 32


def test_offline_boundary():
    # A separate fake receiver validates cf32 conversion and writes BBFRAMEs to
    # the actual inherited descriptor. IQ stdout/log isolation remains exercised.
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        source = folder / "dump.iq"
        source.write_bytes(b"\x00\x01\x00\xff" * 2048)
        size = len(decode.payload(7, 1, "N0CALL"))
        data = decode.payload(7, 16, "N0CALL")[size:10*size]
        header = bytearray([0x72, 0, 0, 0]) + (size*8).to_bytes(2, "big") + bytes(3)
        header.append(decode.crc8(header))
        frames = b"".join(header + data[i:i+size] for i in range(0, len(data), size))
        (folder / "frames").write_bytes(frames)
        receiver = folder / "receiver"
        receiver.write_text("#!" + sys.executable + "\nimport os,sys,struct,pathlib\n"
                            "raw=sys.stdin.buffer.read()\nassert len(raw)==(2048+4096)*8\n"
                            "assert struct.unpack('<ff',raw[:8])==(.5,-.5)\n"
                            "fd=int(sys.argv[sys.argv.index('--fd-bbf')+1])\n"
                            f"os.write(fd,pathlib.Path({str(folder / 'frames')!r}).read_bytes())\n"
                            "print('FRAMELOCK 1\\nMER 18.5\\nFREQ -12000',file=sys.stderr)\n"
                            "for i in range(9): print('S2FEC modcod=7 short=0 pilots=1 ldpc_flips=2 "
                            "ldpc_checks_before=3 ldpc_checks_after=0 bch_errors=1',file=sys.stderr)\n")
        receiver.chmod(0o755)
        cmd = [sys.executable, str(EXAMPLE / "check.py"), str(folder / "pass"), "--dry-run", str(source),
               "--dump-rate", "20e6", "--callsign", "N0CALL", "--frames", "16", "--receiver", str(receiver)]
        subprocess.run(cmd, check=True, capture_output=True, timeout=30)
        r = json.loads((folder / "pass/report.json").read_text())
        assert r["passed"] and r["first_frame"] == 1 and r["payload_frames"] == 9
        assert r["compared_bits"] == len(data)*8 and r["payload_bit_errors"] == 0
        assert r["bch_corrected_bits"] == 9 and r["ldpc_bit_flips"] == 18
        assert r["mer_db"]["median"] == 18.5 and r["frequency_offset_hz"]["last"] == -12000
        assert r["evidence"] == "usb-dump" and (folder / "pass/spectrum.png").stat().st_size > 1000
        bad = bytearray(frames)
        bad[len(header) + 100] ^= 1
        (folder / "frames").write_bytes(bad)
        cmd[2] = str(folder / "fail")
        assert subprocess.run(cmd, capture_output=True, timeout=30).returncode == 1
        r = json.loads((folder / "fail/report.json").read_text())
        assert not r["passed"] and not r["payload_match"] and r["payload_bit_errors"] is None


if __name__ == "__main__":
    test_guards()
    test_readback_and_metrics()
    test_coordinator()
    test_capture_worker()
    test_offline_boundary()
    print("OTA: explicit key/callsign/readback guards, no-key on source failure, offline exact-byte checks pass; no devices")
