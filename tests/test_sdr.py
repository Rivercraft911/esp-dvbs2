"""E200 configuration/capture contracts, using executable fakes only. No IIO access."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import sdr


FAKE = r'''
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ["FAKE_IIO"])
args = sys.argv[1:]
tool = pathlib.Path(sys.argv[0]).name
with (root / "calls.jsonl").open("a") as f:
    f.write(json.dumps([tool, *args]) + "\n")
if args == ["--version"]:
    print(tool + " version: 0.25 (git tag: fake)")
    print("Libiio version: 0.25 (git tag: fake) backends: fake")
    sys.exit(0)
state_file = root / "state.json"
state = json.loads(state_file.read_text()) if state_file.exists() else {}
if tool == "iio_attr":
    if "-s" in args:
        fmt = "be:S12/16>>0" if (root / "bad_format").exists() else "le:S12/16>>0"
        for ch in range(2):
            print(f"dev 'cf-ad9361-lpc', channel 'voltage{ch}' (input, index: {ch}, format: {fmt})")
        sys.exit(0)
    tail = args[args.index("-c") + 2:]
    channel, attr = tail[:2]
    direction = "i" if "-i" in args else "o"
    key = f"{direction}/{channel}/{attr}"
    if len(tail) == 3:
        if (root / "write_error").exists():
            sys.exit(7)
        state[key] = tail[2]
        state_file.write_text(json.dumps(state))
    else:
        value = state.get(key, "unknown")
        if (root / "override.json").exists():
            value = json.loads((root / "override.json").read_text()).get(key, value)
        if attr == "hardwaregain":
            value = str(value) + " dB"
        print(value)
else:
    assert args[args.index("-b") + 1] == args[args.index("-s") + 1]
    assert args[-3:] == ["cf-ad9361-lpc", "voltage0", "voltage1"]
    assert state["o/altvoltage1/powerdown"] == "1"
    if (root / "timeout").exists():
        time.sleep(2)
    if (root / "read_error").exists():
        print("fake refill failed", file=sys.stderr)
        sys.exit(9)
    count = int(args[args.index("-s") + 1])
    if (root / "short").exists():
        count -= 1
    source = root / "samples.iq"
    sys.stdout.buffer.write(source.read_bytes()[:4 * count] if source.exists() else bytes(4 * count))
'''


def fake_tools(root):
    prefix = root / "libiio"
    (prefix / "bin").mkdir(parents=True)
    (prefix / "lib").mkdir()
    for name in ("iio_attr", "iio_readdev"):
        p = prefix / "bin" / name
        p.write_text(f"#!{sys.executable}\n" + FAKE)
        p.chmod(0o755)
    return prefix


def calls(root):
    path = root / "calls.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def rejected(fn, exception=ValueError):
    try:
        fn()
    except exception:
        return
    raise AssertionError("expected refusal")


def test_sdr():
    with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"FAKE_IIO": tmp}):
        root = pathlib.Path(tmp)
        prefix = fake_tools(root)
        device = sdr.SDR(sdr.Settings(gain_db=10), prefix)
        rejected(lambda: device.capture(root / "unset.iq", 16))
        configured = device.configure()
        assert configured["readback"]["i/voltage0/hardwaregain"] == "10 dB"
        assert len(configured["readback"]) == 8
        assert "0.25" in configured["libiio_version"]["iio_readdev"]
        result = device.capture(root / "ok.iq", 16)
        assert result["status"] == "ok" and result["bytes"] == 64
        assert json.loads((root / "ok.iq.json").read_text()) == result
        assert result["host_time"].endswith("+00:00")
        assert len(result["sha256"]) == 64
        assert str(prefix / "lib") in device.env["DYLD_LIBRARY_PATH"]
        log = calls(root)
        writes = [c for c in log if c[0] == "iio_attr" and len(c) == 9]
        assert writes[0][-3:] == ["altvoltage1", "powerdown", "1"]
        assert not any(c[-3:-1] == ["altvoltage1", "powerdown"] and c[-1] != "1" for c in writes)
        capture = [c for c in log if c[0] == "iio_readdev" and "-b" in c]
        assert len(capture) == 1
        assert capture[0][capture[0].index("-b") + 1] == "16"
        rejected(lambda: device.capture(root / "ok.iq", 16), FileExistsError)
        for key, value in [(a[:3], "wrong") for a in device.settings.attributes()]:
            direction, channel, attr = key
            (root / "override.json").write_text(json.dumps({f"{direction[1:]}/{channel}/{attr}": value}))
            rejected(device.configure)
            assert device.metadata is None
        (root / "override.json").unlink()
        (root / "bad_format").touch()
        rejected(device.configure)
        (root / "bad_format").unlink()
        device.configure()
        (root / "override.json").write_text(json.dumps({"o/altvoltage1/powerdown": "0"}))
        rejected(lambda: device.capture(root / "changed.iq", 16))
        assert not (root / "changed.iq").exists()
        assert json.loads((root / "changed.iq.json").read_text())["status"] == "failed"
        (root / "override.json").unlink()
        for failure in ("short", "read_error", "timeout"):
            (root / failure).touch()
            if failure == "timeout":
                device.timeout = .5
            rejected(lambda: device.capture(root / (failure + ".iq"), 16),
                     subprocess.TimeoutExpired if failure == "timeout" else RuntimeError)
            assert json.loads((root / (failure + ".iq.json")).read_text())["status"] == "failed"
            (root / failure).unlink()
        (root / "write_error").touch()
        rejected(device.configure, subprocess.CalledProcessError)
    print("sdr: exact read-backs, TX LO off, scan format, one buffer, sidecar, drift, write/refill/length/timeout failures")


def test_validation():
    for kw in ({"gain_db": float("nan")}, {"rate_hz": 0}, {"port": ""}, {"lo_hz": 1.5}):
        rejected(lambda: sdr.Settings(**kw))
    device = sdr.SDR(sdr.Settings())
    for n in (0, -1, 1.5, 2**32):
        rejected(lambda: device.read_command(n))
    print("sdr: invalid settings/sample counts refused before any command")


if __name__ == "__main__":
    test_validation()
    test_sdr()
