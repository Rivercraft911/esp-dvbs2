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
sys.path.insert(0, str(EXAMPLE))
iio_capture = load("ota_iio_capture", EXAMPLE / "capture_iio.py")
import decode


def rejects(fn, *args):
    try:
        fn(*args)
    except (ValueError, RuntimeError):
        return
    raise AssertionError("invalid evidence accepted")


def test_guards():
    for mhz in (2000,2025,2110,2200,2290,2400,2450):
        args = ota.parser().parse_args(["--mhz",str(mhz)])
        assert ota.settings(args)["frequency"] == mhz * 1e6
    for mhz in (1999,2451):
        rejects(ota.settings,ota.parser().parse_args(["--mhz",str(mhz)]))
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
        # The libiio coordinator reaches the same real decoder boundary, exact
        # payload verifier and spectrum/report writers. Only hardware is replaced.
        def hardware(args, settings, out):
            ota.convert_dump(source, out / "capture.cfile")
            return "N0CALL", 16, dict(backend="libiio", complete=True, overflows=None)
        with patch.object(sys, "argv", ["check.py", str(folder / "rf"), "--backend", "libiio",
                "--uri", "ip:fake", "--key", "--receiver", str(receiver), "--iio-python", sys.executable]), \
                patch.object(ota, "hardware", side_effect=hardware) as run_hardware, \
                patch.object(ota.shutil, "disk_usage", return_value=types.SimpleNamespace(free=10**12)), \
                contextlib.redirect_stdout(None):
            assert ota.main() == 0, (folder / "rf/report.json").read_text()
            run_hardware.reset_mock()
            with patch.object(sys, "argv", ["check.py", str(folder / "low-disk"), "--backend", "libiio",
                    "--uri", "ip:fake", "--key", "--receiver", str(receiver), "--iio-python", sys.executable]), \
                    patch.object(ota.shutil, "disk_usage", return_value=types.SimpleNamespace(free=0)):
                assert ota.main() == 1, (folder / "low-disk/report.json").read_text()
            run_hardware.assert_not_called()
            low_disk = json.loads((folder / "low-disk/report.json").read_text())
            assert not low_disk["passed"] and "insufficient free disk" in low_disk["error"], low_disk
        rf = json.loads((folder / "rf/report.json").read_text())
        assert rf["passed"] and rf["evidence"] == "rf-capture" and rf["capture"]["backend"] == "libiio"
        assert (folder / "rf/spectrum.png").stat().st_size > 1000
        bad = bytearray(frames)
        bad[len(header) + 100] ^= 1
        (folder / "frames").write_bytes(bad)
        cmd[2] = str(folder / "fail")
        assert subprocess.run(cmd, capture_output=True, timeout=30).returncode == 1
        r = json.loads((folder / "fail/report.json").read_text())
        assert not r["passed"] and not r["payload_match"] and r["payload_bit_errors"] is None


def test_iio_guards():
    base = ["--backend", "libiio", "--uri", "ip:example.invalid"]
    for rate, fs in (("1m", 2.5e6), ("5m", 10e6), ("10m", 20e6),
                     ("20m", 40e6), ("26m667", 160e6 / 3), ("33m333", 61.44e6)):
        extra = (["--burst", "16000000"] if fs > 10e6 else [])
        if rate == "33m333":
            extra += ["--upsample", "2"]
        output = subprocess.check_output([sys.executable, "-S", str(EXAMPLE / "check.py"),
                                          "--plan", *base, "--rate", rate, *extra])
        plan = json.loads(output)
        assert plan["rf"]["backend"] == "libiio" and plan["rf"]["sample_rate"] == fs
        assert "capture_iio.py" in plan["capture_command"][1]
        assert plan["rf"]["burst_samples"] == (16_000_000 if fs > 10e6 else 0)
        assert plan["max_capture_bytes"] == (128_000_000 if fs > 10e6 else 42 * fs * 8)
        if rate == "1m":
            assert plan["board_command"] == "dvb 2420 7 30 0 128"
    for extra in (["--uri", "ip:"], ["--uri", "usb:"], ["--uri", "serial:/dev/fake"],
                  ["--args", "addr=fake"], ["--rate", "10m"], ["--sample-rate", "nan"],
                  ["--sample-rate", "5e6"], ["--bandwidth", "1e6"], ["--burst", "-1"],
                  ["--burst", "16000001"], ["--upsample", "0"], ["--burst-margin", "nan"]):
        rejects(ota.settings, ota.parser().parse_args([*base, *extra]))
    for extra in (["--key"], ["--uri", "ip:example.invalid"]):
        r = subprocess.run([sys.executable, "-S", str(EXAMPLE / "check.py"),
                            "--backend", "libiio", *extra], capture_output=True)
        assert r.returncode == 2 and b"ModuleNotFoundError" not in r.stderr
    r = subprocess.run([sys.executable, "-S", str(EXAMPLE / "capture_iio.py"), "missing"],
                        capture_output=True)
    assert r.returncode == 2 and b"requires explicit --key" in r.stderr


def test_iio_worker():
    import numpy as np
    assert iio_capture.number("-89.750000 dB", "dB") == -89.75
    assert iio_capture.number(" 30.000000 dB\n", "dB") == 30
    assert iio_capture.number("1\n") == 1
    for value in ("", "nan dB", "inf dB", "-89.75 dBm", "-89.75 dB extra"):
        rejects(iio_capture.number, value, "dB")
    rejects(iio_capture.number, "1 dB")  # Powerdown/raw do not carry gain units.
    # Exercise the actual worker against a fake pylibiio context. Every attempt to
    # create a buffer asserts the TX state and RX device, including additional TX channels.
    success = (None, "burst", "ad9364", "rx_freq_round")
    for fault in (*success, "lo", "gain", "dds", "write", "nan", "missing", "missing_tx0",
                  "missing_gain0", "missing_gain1", "rx_rate", "rx_freq", "manual", "format", "short", "refill", "post_tx"):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            s = ota.settings(ota.parser().parse_args(["--backend", "libiio", "--uri", "ip:fake"]))
            s["pre_seconds"] = 0  # One refill suffices for this loop test.
            if fault == "burst":
                s["burst_samples"] = 1024
            attrs, created, kernel = {}, [], []

            class Attr:
                def __init__(self, name, value="0"):
                    self.name, self.stored = name, value
                    attrs[name] = self
                @property
                def value(self):
                    if fault == "nan" and self.name == "lo":
                        return "nan"
                    if self.name == fault:
                        return "0.25"  # Ignored writes / unexpected hardware readback.
                    if fault == "rx_rate" and self.name == "sampling_frequency":
                        return "9900000"
                    if fault in ("rx_freq_round", "rx_freq") and self.name == "frequency":
                        return str(int(self.stored) - (2 if fault == "rx_freq_round" else 1000))
                    if fault == "manual" and self.name == "gain_control_mode":
                        return "slow_attack"
                    if fault == "post_tx" and created and self.name == "lo":
                        return "0"
                    if self.name in ("tx0", "gain", "hardwaregain"):
                        return f"{float(self.stored):.6f} dB"
                    return self.stored
                @value.setter
                def value(self, value):
                    if fault == "write" and self.name == "lo":
                        raise OSError("injected attribute write failure")
                    if self.name == "hardwaregain":
                        assert value != "-89.75", "TX attenuation must never target the RX input channel"
                    assert not self.name.startswith("aux"), "auxiliary DACs must remain untouched"
                    self.stored = value

            class Channel:
                def __init__(self, name, output, attributes, index=0, scan=False):
                    self.id, self.output, self.index, self.scan_element = name, output, index, scan
                    self.attrs = {k: Attr(v) for k, v in attributes.items()}
                    self.enabled = False
                    self.data_format = types.SimpleNamespace(length=16, bits=12,
                        shift=1 if fault == "format" else 0, is_signed=True, is_be=False, repeat=1)

            class Device:
                sample_size = 4
                def __init__(self, name, channels):
                    self.name, self.channels = name, channels
                def find_channel(self, name, output=False):
                    return next((c for c in self.channels if c.id == name and c.output == output), None)
                def set_kernel_buffers_count(self, count):
                    kernel.append(count)

            phy = Device("ad9361-phy", [
                Channel("altvoltage1", True, {"powerdown": "lo"}),
                Channel("altvoltage0", True, {"frequency": "frequency"}),
                Channel("voltage0", True, {"hardwaregain": "tx0"}),
                Channel("voltage1", True, {"hardwaregain": "gain"}),
                Channel("voltage0", False, {k: k for k in
                        ("sampling_frequency", "rf_bandwidth", "hardwaregain", "gain_control_mode")}),
                # AD936x exposes auxiliary DACs with output voltage IDs too.
                Channel("voltage2", True, {"raw": "aux2_raw", "scale": "aux2_scale"}),
                Channel("voltage3", True, {"raw": "aux3_raw", "scale": "aux3_scale"}),
                Channel("voltage2", False, {"raw": "aux_adc_raw", "scale": "aux_adc_scale"})])
            attrs["hardwaregain"].stored = "47.25"  # Distinguish input voltage0 from TX voltage0.
            if fault == "ad9364":
                # Pluto v0.34 in AD9364 mode: one RF TX channel, four DDS tones.
                phy.channels = [c for c in phy.channels if not (c.output and c.id == "voltage1")]
            if fault == "missing_tx0":
                phy.channels = [c for c in phy.channels if not (c.output and c.id == "voltage0")]
            if fault in ("missing_gain0", "missing_gain1"):
                phy.find_channel("voltage" + fault[-1], True).attrs.pop("hardwaregain")
            tone_count = 4 if fault == "ad9364" else 8
            dds = Device("cf-ad9361-dds-core-lpc", [Channel(f"altvoltage{i}", True,
                {"raw": "dds" if i == tone_count - 1 else f"dds{i}"}) for i in range(tone_count)])
            rx = Device("cf-ad9361-lpc", [Channel(f"voltage{i}", False, {}, i, True) for i in range(4)])
            devices = {d.name: d for d in (phy, dds, rx)}
            if fault == "missing":
                del devices[dds.name]
            context = types.SimpleNamespace(find_device=devices.get, description="fake Pluto",
                                           attrs={"serial": "fake"}, set_timeout=lambda ms: None)

            class Buffer:
                step = 4
                def __init__(self, device, count, cyclic):
                    assert device is rx and cyclic is False
                    assert iio_capture.Radio(context).verify_tx()["lo_powerdown"] == 1
                    assert [c.enabled for c in rx.channels] == [True, True, False, False]
                    if fault == "burst":
                        assert (out / "trigger").exists() and kernel == [1]
                    else:
                        assert kernel == [4]
                    self.count = count
                    created.append(self)
                def refill(self):
                    if fault == "refill":
                        raise OSError("injected overflow / transport failure")
                    (out / "stop").touch()
                def read(self):
                    assert not (out / "capture.cfile").exists(), "conversion must wait for RX cleanup"
                    data = np.tile(np.array([-2048, 1024], dtype="<i2"), self.count).tobytes()
                    return data[:-4] if fault == "short" else data
                def cancel(self):
                    created.append("cancel")

            def write(path, value):
                capture.write_json(path, value)
                if path.name == "ready.json" and fault == "burst":
                    assert not created  # Ready means armed, with no early queued samples.
                    (out / "trigger").touch()
            module = types.SimpleNamespace(Context=lambda uri: context, Buffer=Buffer, version=(0, 26, "fake"))
            with patch.dict(sys.modules, iio=module), patch.object(iio_capture, "write_json", side_effect=write):
                assert iio_capture.capture(s, out) == (0 if fault in success else 1), fault
            report = json.loads((out / "capture.json").read_text())
            assert report["overflows"] is None  # Do not invent a zero hardware overflow count.
            if fault in success:
                assert report["samples"] == (1024 if fault == "burst" else 1048576)
                z = np.fromfile(out / "capture.cfile", dtype="<c8")
                assert np.all(z == -1 + .5j) and created[-1] == "cancel"
                assert report["buffer_samples"] == (1024 if fault == "burst" else 1048576)
                assert not (out / "capture.i16").exists()
                assert report["conversion_seconds"] >= 0
                assert report["tx_after_capture"]["dds_raw"][f"altvoltage{tone_count-1}"] == 0
                assert report["tx_final"]["hardwaregain"] == (
                    {"voltage0": -89.75} if fault == "ad9364" else {"voltage0": -89.75, "voltage1": -89.75})
                assert report["actual"]["gain"] == s["gain"]
            elif fault not in ("short", "refill", "post_tx"):
                assert not created and not (out / "ready.json").exists(), fault
                if fault in ("missing_gain0", "missing_gain1"):
                    assert f"output voltage{fault[-1]}.hardwaregain" in report["error"]
                    assert attrs["lo"].stored == "1"
            else:
                assert created[-1] == "cancel"
            # No failure cleanup ever re-enables the SDR transmitter.
            assert attrs["lo"].stored in ("0", "1")
            if created:
                assert attrs["lo"].stored == "1" and attrs["tx0"].stored == "-89.75"
                assert attrs["gain"].stored == ("0" if fault == "ad9364" else "-89.75")


def test_upsample():
    import numpy as np
    from scipy.signal import resample_poly
    rng = np.random.default_rng(2718)
    z = (rng.normal(size=263000) + 1j*rng.normal(size=263000)).astype("<c8")
    with tempfile.TemporaryDirectory() as tmp:
        source, target = Path(tmp) / "native.cfile", Path(tmp) / "decode.cfile"
        z.tofile(source)
        ota.upsample(source, target, 2)
        actual = np.fromfile(target, dtype="<c8")
        assert np.array_equal(actual, resample_poly(z, 2, 1))
        assert np.array_equal(np.fromfile(source, dtype="<c8"), z)


def test_iio_coordinator():
    import morse
    args = ota.parser().parse_args(["--backend", "libiio", "--uri", "ip:fake", "--key",
                                    "--rate", "10m", "--burst", "16000000"])
    row = dict(cmd="dvb", callsign="N0CALL", status="ESP_OK", modcod=7, symbol_rate=10000000,
               payload_source="xorshift32", encoder_failed=False, underruns=0, frames_played=15,
               frames_encoded=18, freq_mhz=2420, seconds=30, gain=0, peak_level=128,
               elapsed_samples=1200000000, dac_rate=40000000, id_samples=208800000)
    sent = []
    def run(port, text, **kw):
        assert not kw["reset"]
        sent.append(text)
        return [{"callsign": "N0CALL"}] if text == "info" else [row] if text.startswith("dvb ") else [{}]
    board = types.SimpleNamespace(run=run, reserve_port=lambda port: contextlib.nullcontext())
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        class Process:
            finished = False
            def poll(self):
                return 0 if self.finished else None
            def terminate(self):
                self.finished = True
            def wait(self, **kw):
                assert (out / "trigger").exists() and (out / "stop").exists()
                now = ota.time.monotonic()
                capture.write_json(out / "capture.json", dict(complete=True, overflows=None, samples=16000000,
                    buffer_started_monotonic=now, buffer_filled_monotonic=now))
                self.finished = True
                return 0
        def start(command, **kw):
            assert "capture_iio.py" in command[1] and "--key" in command
            capture.write_json(out / "ready.json", dict(armed=True))
            return Process()
        s = ota.settings(args)
        # Eliminate real waiting; the timing bounds themselves are tested below.
        s["burst_margin"] = -morse.duration("N0CALL")
        with patch.dict(sys.modules, board=board), patch.object(ota.subprocess, "Popen", side_effect=start):
            call, frames, report = ota.hardware(args, s, out)
            assert (call, frames) == ("N0CALL", 18)
            assert sent == ["info", "spiinput off", "dvb 2420 7 10m 30 0 128"]
            assert report["after_leading_id"] and report["before_trailing_id"]
            sent.clear()
            # Real readiness failure prevents keying, for this backend as for UHD.
            with patch.object(ota, "wait_ready", side_effect=RuntimeError("TX inhibit failed")):
                rejects(ota.hardware, args, s, out)
            assert sent == ["info", "spiinput off"]
            sent.clear()
            short = ota.parser().parse_args(["--backend", "libiio", "--uri", "ip:fake", "--key",
                "--rate", "10m", "--burst", "16000000", "--seconds", "13"])
            rejects(ota.hardware, short, ota.settings(short), out)
            assert sent == ["info"]

    s = dict(burst_samples=16000000, burst_delay_seconds=7.52)
    report = dict(samples=16000000, buffer_started_monotonic=108, buffer_filled_monotonic=110)
    ota.check_burst_timing(report, row, s, 100, 131)
    for key, value in (("samples", 15999999), ("buffer_started_monotonic", 105),
                       ("buffer_filled_monotonic", 126), ("buffer_started_monotonic", float("nan"))):
        rejects(ota.check_burst_timing, dict(report, **{key: value}), row, s, 100, 131)


if __name__ == "__main__":
    test_guards()
    test_readback_and_metrics()
    test_coordinator()
    test_capture_worker()
    test_offline_boundary()
    test_iio_guards()
    test_iio_worker()
    test_upsample()
    test_iio_coordinator()
    print("OTA: UHD/libiio guards, TX inhibits, streaming/burst failures, exact-byte checks and upsampling pass; no devices")
