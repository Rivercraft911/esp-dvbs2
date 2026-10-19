"""Fake pylibiio, live process boundaries and coordinator payload proof; no devices."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import types
from unittest.mock import patch

import numpy as np
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "examples/live-video"
sys.path.insert(0, str(HERE))
import check_e200 as coordinator
import iio_stream as worker
import receive
from make_ts import make_ts


def rejects(fn, *args):
    try:
        fn(*args)
    except (ValueError, RuntimeError):
        return
    raise AssertionError("bad input/evidence accepted")


def fake_iio(fault=None, payload=None):
    events, attributes, buffers = [], {}, []

    class Attr:
        def __init__(self, name):
            self.name, self.stored = name, "0"
            attributes[name] = self
        @property
        def value(self):
            events.append(("read", self.name))
            if self.name == fault or (fault == "post_tx" and buffers and self.name == "lo"):
                return "0.25"
            if fault == "coerce" and self.name == "sampling_frequency":
                return "3000000"
            return self.stored
        @value.setter
        def value(self, value):
            if self.name == "frequency":
                # All inhibits were read back before the first RX setup write.
                names = ("lo", "tx0", "dds7") if fault == "one_tx" else ("lo", "tx0", "tx1", "dds7")
                assert all(("read", name) in events for name in names)
                assert inhibited()
            if fault == "write" and self.name == "lo":
                raise OSError("attribute write failed")
            events.append(("write", self.name, value))
            self.stored = value

    class Channel:
        def __init__(self, name, output=False, attrs=None, index=0, scan=False):
            self.id, self.output, self.index, self.scan_element = name, output, index, scan
            self.attrs = {key: Attr(value) for key, value in (attrs or {}).items()}
            self.enabled = False
            self.data_format = types.SimpleNamespace(length=16, bits=12, shift=0,
                is_signed=True, is_be=fault == "format", repeat=1)

    class Device:
        sample_size = 4
        def __init__(self, name, channels):
            self.name, self.channels = name, channels
        def find_channel(self, name, output=False):
            return next((c for c in self.channels if c.id == name and c.output == output), None)
        def set_kernel_buffers_count(self, count):
            assert count == 4
            events.append(("kernel", count))

    phy = Device("ad9361-phy", [Channel("altvoltage1", True, {"powerdown": "lo"}),
        Channel("altvoltage0", True, {"frequency": "frequency"}),
        Channel("voltage0", True, {"hardwaregain": "tx0"}),
        Channel("voltage1", True, {"hardwaregain": "tx1"}),
        Channel("voltage0", attrs={k: k for k in ("sampling_frequency", "rf_bandwidth", "gain_control_mode", "hardwaregain")}),
        Channel("voltage2", True, {"raw": "aux"})])
    dds = Device("cf-ad9361-dds-core-lpc", [Channel(f"altvoltage{k}", True, {"raw": f"dds{k}"}) for k in range(8)])
    rx = Device("cf-ad9361-lpc", [Channel(f"voltage{k}", index=k, scan=True) for k in range(4)])
    devices = {d.name: d for d in (phy, dds, rx)}
    if fault == "missing":
        devices.pop(dds.name)
    if fault == "missing_gain":
        phy.find_channel("voltage1", True).attrs.clear()
    if fault == "one_tx":
        phy.channels.remove(phy.find_channel("voltage1", True))
    context = types.SimpleNamespace(find_device=devices.get, description="fake E200", attrs={},
                                    set_timeout=lambda ms: events.append(("timeout", ms)))

    def inhibited():
        return (attributes["lo"].stored == "1" and attributes["tx0"].stored == "-89.75" and
                (fault == "one_tx" or attributes["tx1"].stored == "-89.75") and
                all(attributes[f"dds{k}"].stored == "0" for k in range(8)))

    class Buffer:
        step = 8 if fault == "stride" else 4
        def __init__(self, device, count, cyclic):
            assert device is rx and cyclic is False and inhibited()
            assert [c.enabled for c in rx.channels] == [True, True, False, False]
            assert sum(e == ("read", "lo") for e in events) >= 3
            self.count = count
            self.next = time.monotonic()
            buffers.append(self)
            events.append(("buffer",))
        def refill(self):
            if fault == "refill":
                raise OSError("transport failure")
            if fault != "fast":
                self.next += self.count / float(attributes["sampling_frequency"].stored)
                time.sleep(max(0, self.next-time.monotonic()))
            if fault == "slow":
                time.sleep(.15)
            events.append(("refill",))
        def read(self):
            raw = (np.tile(np.array([-2048, 1024], dtype="<i2"), self.count).tobytes()
                   if payload is None else (payload*self.count)[:self.count*4])
            return raw[:-4] if fault == "short" else raw
        def cancel(self):
            events.append(("cancel",))
            if fault == "cancel":
                raise OSError("cancel failed")

    def connect(uri):
        assert uri == "ip:fake"
        events.append(("context", uri))
        return context
    return types.SimpleNamespace(Context=connect, Buffer=Buffer, version=(0, 26, "fake")), events, attributes


def args(out, *extra):
    a = worker.parser().parse_args(["--uri", "ip:fake", "--freq", "2420e6", "--rate", "2.5e6",
        "--symbol-rate", "1e6", "--seconds", "1", "--buffer-samples", "262144", "--probe",
        "--report", str(out / "capture.json"), "--ready", str(out / "ready.json"), *extra])
    worker.validate(a)
    return a


def test_worker():
    assert worker.Radio.__module__ == "capture_iio"  # shared inhibit code, unchanged
    for fault in (None, "one_tx", "lo", "tx1", "dds7", "missing", "missing_gain", "write", "coerce",
                  "format", "stride", "short", "refill", "post_tx", "cancel", "slow"):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            module, events, attributes = fake_iio(fault)
            with contextlib.redirect_stderr(io.StringIO()):
                code = worker.stream(args(out), module)
            report = json.loads((out / "capture.json").read_text())
            assert (code == 0) == (fault in (None, "one_tx")), (fault, report)
            assert report["overflows"] is None and "unknown" in report["continuity"]
            assert not any(e[:2] == ("write", "aux") for e in events)
            if fault in (None, "one_tx"):
                assert report["complete"] and report["samples"] >= 2.5e6
                assert report["delivered_samples"] == report["samples"]
                assert (out / "ready.json").exists()
                cancel_at = events.index(("cancel",))
                assert ("read", "lo") in events[cancel_at+1:]
                assert report["tx_final"]["lo_powerdown"] == 1
            elif fault in ("lo", "tx1", "dds7", "missing", "missing_gain", "write", "coerce", "format"):
                assert ("buffer",) not in events and not (out / "ready.json").exists()
            if ("buffer",) in events:
                assert ("cancel",) in events
                assert attributes["lo"].stored == "1" and attributes["tx0"].stored == "-89.75"


def test_interpolation():
    rng = np.random.default_rng(76)
    values = rng.integers(-2048, 2048, size=(3001, 2), dtype=np.int16)
    interpolation = worker.Interpolator(2)
    chunks = [values[:750], values[750:1531], values[1531:]]
    raw = b"".join(interpolation.push(v.astype("<i2").tobytes()) for v in chunks) + interpolation.push(None)
    expected = np.clip(np.rint(resample_poly(values.astype(np.float32), 2, 1, axis=0)), -32768, 32767).astype("<i2")
    assert np.array_equal(np.frombuffer(raw, dtype="<i2").reshape(-1, 2), expected)


def test_backpressure():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        fifo = out / "iq"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)  # a consumer that never reads
        a = args(out)
        a.probe, a.output = False, str(fifo)
        module, events, _ = fake_iio()
        before = time.monotonic()
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                code = worker.stream(a, module)
        finally:
            os.close(fd)
        report = json.loads(a.report.read_text())
        assert code == 1 and time.monotonic()-before < 4
        assert "queue" in report["error"] or "stalled" in report["error"]
        assert report["queue_depth_max"] == 4 and ("cancel",) in events
        assert report["tx_final"]["lo_powerdown"] == 1


def test_guards_and_plans():
    base = [sys.executable, "-S", str(HERE / "receive.py"), "--backend", "libiio",
            "--uri", "ip:fake", "--freq", "2420e6", "--rate", "10e6", "--symbol-rate", "5e6", "--modcod", "13"]
    for extra in (["--uri", "ip:"], ["--freq", "nan"], ["--gain", "nan"], ["--rate", "1e6"],
                  ["--rate", "nan"], ["--seconds", "nan"], ["--bandwidth", "1e6"], ["--buffer-samples", "1"],
                  ["--uri", "auto:"], ["--args", "fake"]):
        result = subprocess.run([*base, *extra, "--plan"], capture_output=True)
        assert result.returncode == 2 and b"ModuleNotFoundError" not in result.stderr, extra
    for rate, rs, mc, factor in ((10e6, 5e6, 13, 1), (20e6, 10e6, 7, 1), (15e6, 10e6, 7, 2)):
        output = subprocess.check_output([*base, "--rate", str(rate), "--symbol-rate", str(rs),
            "--modcod", str(mc), "--upsample", str(factor), "--plan"])
        plan = json.loads(output)
        decoder = plan["decoder"]
        assert "--s16" in decoder and decoder[decoder.index("--sr-tol")+1] == "0.0005"
        assert decoder[decoder.index("-f")+1] == str(rate*factor)
        assert decoder[decoder.index("--modcods")+1] == str(1 << mc)
    for action in ("probe", "keyed"):
        cmd = [sys.executable, "-S", str(HERE / "check_e200.py"), action, "unused", "--uri", "ip:fake", "--mhz", "2420"]
        assert subprocess.run([*cmd, "--plan"], capture_output=True).returncode == 0
        if action == "keyed":
            result = subprocess.run(cmd, capture_output=True)
            assert result.returncode == 2 and b"requires --key" in result.stderr


def test_timing_and_rate_gate():
    timing = worker.Timing(10e6, 1000000, 0)
    timing.refill(0, .3)  # slow start-up is allowed
    timing.refill(.3, .4)
    timing.refill(.4, .6)  # one slow refill within kernel buffering
    assert timing.passed() and timing.rows["slow_refills"] == 1
    timing.refill(.6, .9)
    assert not timing.passed()
    # Individually plausible refill times can still build an excessive lag.
    timing = worker.Timing(10e6, 1000000, 0)
    for k in range(20):
        timing.refill(k*.14, (k+1)*.14)
    assert not timing.passed() and timing.rows["slow_refills"] == 0
    a = coordinator.parser().parse_args(["keyed", "unused", "--uri", "ip:fake", "--mhz", "2420"])
    probe = dict(uri=a.uri, frequency=2420e6, rx_gain=a.rx_gain, rates=[dict(requested_rate=rate,
        complete=True, host_timing_passed=True, exit=0, requested_seconds=30, sample_seconds=30) for rate in coordinator.RATES])
    assert coordinator.choose_rate(probe, a) == 10e6
    a.modcod = 7
    assert coordinator.choose_rate(probe, a) == 20e6
    probe["rates"][2]["host_timing_passed"] = False
    assert coordinator.choose_rate(probe, a) == 15e6
    rx, _ = coordinator.keyed_commands(a, 15000000)
    assert rx[rx.index("--upsample")+1] == "1"
    probe["rates"][1]["complete"] = False
    rejects(coordinator.choose_rate, probe, a)
    probe["rates"][1]["complete"] = True
    probe["uri"] = "ip:elsewhere"
    rejects(coordinator.choose_rate, probe, a)


def evidence(out, source):
    stats = dict(source_eof=True, complete=True, terminal_nop=True, write_uncertain=False,
        errors=[], payload_bytes=len(source), ts_packets=len(source)//188, crc_chain=1234, messages=100,
        next_seq=100, controls=0)
    usb = dict(ended=True, payload_bytes=len(source), ts_packets=len(source)//188, crc_chain=1234,
        messages=100, nops=1, next_seq=101, us=1000000, mbps=3.2,
        **{k: 0 for k in ("short", "header", "crc", "sync", "lost", "back", "wrong_type", "partial_bytes")})
    row = dict(cmd="dvb", modcod=13, symbol_rate=5000000, freq_mhz=2420, seconds=60, gain=0,
        payload_source="usb", input="ts", callsign="N0CALL", status="ESP_OK", encoder_failed=False,
        frames_encoded=100, underruns=1000, payload_bytes_per_frame=5370, frame_symbols=22194, usb=usb,
        late_fills=0, late_switches=0, fatal_late=0, min_fill_slack_cycles=100)
    sender = dict(sender=stats, records=[row], errors=[])
    (out / "sender.json").write_text(json.dumps(sender))
    (out / "source.ts").write_bytes(source)
    (out / "received.ts").write_bytes(source)
    (out / "capture.json").write_text(json.dumps(dict(complete=True, host_timing_passed=True)))
    return sender


def test_payload_proof():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        a = coordinator.parser().parse_args(["keyed", str(out), "--uri", "ip:fake", "--mhz", "2420"])
        source = make_ts(5)
        sender = evidence(out, source)
        result = coordinator.verify(out, a)
        assert result["passed"] and result["exact_reference_match"] and result["continuity_errors"] == 0
        # Dropping exactly 16 non-null packets can hide behind wrapping CCs;
        # null loss is also invisible to counters. Both must fail exact bytes.
        packets = [source[k:k+188] for k in range(0, len(source), 188)]
        null = next(k for k, p in enumerate(packets) if p[1] & 31 == 31 and p[2] == 255)
        (out / "received.ts").write_bytes(b"".join(packets[:null]+packets[null+1:]))
        rejects(coordinator.verify, out, a)
        video = [k for k, p in enumerate(packets) if ((p[1] & 31) << 8 | p[2]) == 256]
        removed = set(video[1:17])
        (out / "received.ts").write_bytes(b"".join(p for k, p in enumerate(packets) if k not in removed))
        rejects(coordinator.verify, out, a)
        # Byte corruption in a null packet has valid framing and no CC symptom.
        changed = bytearray(source)
        changed[null*188+50] ^= 1
        (out / "received.ts").write_bytes(changed)
        rejects(coordinator.verify, out, a)
        (out / "received.ts").write_bytes(source[:len(source)//188//2*188])
        rejects(coordinator.verify, out, a)
        (out / "received.ts").write_bytes(source)
        sender["sender"]["source_eof"] = False
        (out / "sender.json").write_text(json.dumps(sender))
        rejects(coordinator.verify, out, a)


def test_coordinator():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        a = coordinator.parser().parse_args(["probe", str(out), "--uri", "ip:fake", "--mhz", "2420"])
        started = []

        class Probe:
            def __init__(self, cmd, **kwargs):
                assert "--probe" in cmd and Path(cmd[1]).name == "iio_stream.py"
                rate = int(cmd[cmd.index("--rate")+1])
                started.append(rate)
                self.returncode = 1 if rate == 20000000 else 0
                Path(cmd[cmd.index("--report")+1]).write_text(json.dumps(dict(
                    complete=not self.returncode, host_timing_passed=not self.returncode,
                    requested_seconds=30, sample_seconds=30)))
            def wait(self, **kwargs):
                return self.returncode
            def poll(self):
                return self.returncode
        with patch.object(coordinator.subprocess, "Popen", Probe), contextlib.redirect_stdout(io.StringIO()):
            assert coordinator.probe(a) == 0
        assert started == list(coordinator.RATES)
        assert json.loads((out / "report.json").read_text())["qpsk_rates"] == [15000000]

    for receiver_fails in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            a = coordinator.parser().parse_args(["keyed", str(out), "--uri", "ip:fake", "--mhz", "2420", "--port", "fake"])
            launches = []

            class Process:
                def __init__(self, cmd, **kwargs):
                    self.receiver = "--backend" in cmd
                    launches.append("rx" if self.receiver else "tx")
                    self.returncode = None
                    self.polls = 0
                    if self.receiver and not receiver_fails:
                        (out / "ready.json").write_text('{"ready":true}')
                    if not self.receiver:
                        assert (out / "ready.json").exists() and "--key" in cmd
                        evidence(out, (out / "source.ts").read_bytes())
                def poll(self):
                    self.polls += 1
                    if self.receiver:
                        self.returncode = 1 if receiver_fails else 0 if (out / "stop").exists() else None
                    elif self.polls > 1:
                        self.returncode = 0
                    return self.returncode
                def wait(self, **kwargs):
                    assert (out / "stop").exists()
                    self.returncode = 0
                    return 0
            with patch.object(coordinator.subprocess, "Popen", Process), patch.object(coordinator.time, "sleep"), \
                    contextlib.redirect_stdout(io.StringIO()):
                assert coordinator.keyed(a, 10000000) == int(receiver_fails)
            assert launches == (["rx"] if receiver_fails else ["rx", "tx"])
            assert json.loads((out / "report.json").read_text())["passed"] is not receiver_fails


def test_live_processes():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        # A module in a fresh child process exercises the real receive.py and
        # worker, FIFO, report, EOF and saved TS path without an IIO installation.
        (out / "iio.py").write_text("from test_live_iio import fake_iio\n"
            "_fake, _events, _attrs = fake_iio()\nContext=_fake.Context\nBuffer=_fake.Buffer\nversion=_fake.version\n")
        decoder = out / "decoder"
        decoder.write_text("#!"+sys.executable+"\nimport sys,shutil\nshutil.copyfileobj(sys.stdin.buffer,sys.stdout.buffer)\n")
        decoder.chmod(0o755)
        env = dict(os.environ, PYTHONPATH=os.pathsep.join((str(out), str(ROOT / "tests"))))
        cmd = [sys.executable, str(HERE / "receive.py"), "--backend", "libiio", "--uri", "ip:fake",
            "--freq", "2420e6", "--rate", "2.5e6", "--symbol-rate", "1e6", "--seconds", "1",
            "--receiver", str(decoder), "--capture-report", str(out / "capture.json"),
            "--buffer-samples", "262144", "--output", str(out / "received.ts")]
        result = subprocess.run(cmd, env=env, capture_output=True, timeout=15)
        # The IQ-copy decoder deliberately produces a non-188 byte count;
        # receive.py rejects it, but its binary output is otherwise exact.
        assert result.returncode == 1 and b"TS bytes=" in result.stderr, result.stderr
        report = json.loads((out / "capture.json").read_text())
        expected = np.tile(np.array([-2048, 1024], dtype="<i2"), report["samples"]).tobytes()+bytes(4096*4)
        assert (out / "received.ts").read_bytes() == expected
        assert report["complete"] and report["tx_final"]["lo_powerdown"] == 1
        # Successful TS saving and simultaneous player tee through the same
        # real libiio process boundary; diagnostics must stay outside the TS.
        source = make_ts(5)
        (out / "source.ts").write_bytes(source)
        decoder.write_text("#!"+sys.executable+"\nimport pathlib,sys,shutil\n"
            "sys.stdin.buffer.read(4096)\n"
            f"sys.stdout.buffer.write(pathlib.Path({str(out / 'source.ts')!r}).read_bytes())\n"
            "sys.stdout.buffer.flush()\n"
            "print('decoder status', file=sys.stderr)\n"
            "with open('/dev/null','wb') as f: shutil.copyfileobj(sys.stdin.buffer,f)\n")
        player = out / "player.py"
        player.write_text("import pathlib,sys\n"
            f"pathlib.Path({str(out / 'played.ts')!r}).write_bytes(sys.stdin.buffer.read())\n")
        (out / "capture.json").unlink()
        cmd[cmd.index("--output")+1] = str(out / "saved.ts")
        result = subprocess.run([*cmd, "--player", f"{sys.executable} {player}"],
                                env=env, capture_output=True, timeout=15)
        assert result.returncode == 0, result.stderr
        assert (out / "saved.ts").read_bytes() == source == (out / "played.ts").read_bytes()
        assert b"decoder status" in result.stderr


def test_model_receiver(receiver):
    """Optional independent LeanDVB check of native ADC scale and both MODCODs."""
    from dry_run import rrc
    from scipy.signal import upfirdn
    sys.path.insert(0, str(ROOT / "host"))
    import dvbs2 as d
    source = make_ts(5)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        for mc, code, modulation, rs in ((13, "2/3", "8psk", 5e6), (7, "3/4", "qpsk", 10e6)):
            phases, total = [], 0
            size = (d.code(code).kbch-80)//8
            dummy = d.dummy_phases()
            for n, bb in enumerate(d.ts_bbframes(np.frombuffer(source, np.uint8), code)):
                fec = d.ldpc_encode_groups(d.bch_encode_bytes(d.bb_scramble(bb), code), code)
                frame = d.plframe_phases(fec, code, pilots=True, modulation=modulation)
                phases.append(frame)
                total += len(frame)
                # USB intake: dummy-heavy data separated by valid PL frames.
                target = (n+1)*size*8*rs/3.2e6
                while total+len(dummy) <= target:
                    phases.append(dummy)
                    total += len(dummy)
            symbols = d.symbols_from_phases(np.concatenate(phases))
            z = .5*upfirdn(rrc(), symbols, up=2)
            samples = np.column_stack((z.real, z.imag))*2048
            cases = [(int(2*rs), 1, samples)]
            if mc == 7:
                cases.append((15000000, 2, resample_poly(samples, 3, 4, axis=0)))
            for rate, factor, samples in cases:
                raw = np.clip(np.rint(samples), -32768, 32767).astype("<i2").tobytes()
                if factor == 2:
                    interpolation = worker.Interpolator(2)
                    raw = b"".join(interpolation.push(raw[k:k+4*262144])
                                   for k in range(0, len(raw), 4*262144)) + interpolation.push(None)
                iq = out / f"{mc}-{rate}.i16"
                iq.write_bytes(raw+bytes(4096*4))
                target = iq.with_suffix(".ts")
                command = [sys.executable, str(HERE / "receive.py"), "--iq", str(iq), "--format", "i16",
                    "--rate", str(rate*factor), "--symbol-rate", str(rs), "--modcod", str(mc),
                    "--receiver", receiver, "--output", str(target)]
                result = subprocess.run(command, capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr.decode(errors="replace")[-3000:]
                got = target.read_bytes()
                report = coordinator.check(got, source)
                assert report["exact_reference_match"] and len(got) >= .9*len(source)
                print(f"model MODCOD {mc}, {rate/1e6:g} MS/s x{factor}: {len(got)/len(source):.1%} exact TS; zero CC errors")


def test_processor_delivery():
    """Readiness follows actual writes, including the real finite tail."""
    import hashlib

    class Delay:
        def __init__(self):
            self.pending, self.eof = b"", 0
        def push(self, raw):
            previous, self.pending = self.pending, b"" if raw is None else raw
            self.eof += raw is None
            return previous

    for fault in (None, "refill", "short", "post_tx", "consumer", "processor"):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            a = args(out)
            a.probe, a.output = False, str(out/"iq")
            Path(a.output).touch()
            module, events, _ = fake_iio(fault)
            delay = Delay()
            expected, got = hashlib.sha256(), hashlib.sha256()
            count, ready = [0], []
            original_read = module.Buffer.read
            def read(buffer):
                raw = original_read(buffer)
                expected.update(raw)
                return raw
            module.Buffer.read = read
            def write(fd, raw, failed):
                if raw and fault == "consumer":
                    raise RuntimeError("consumer failure")
                if raw:
                    got.update(raw)
                    count[0] += len(raw)//4
            original_push = delay.push
            def push(raw):
                if raw is not None and fault == "processor":
                    raise ValueError("processor failure")
                return original_push(raw)
            delay.push = push
            original_json = worker.write_json
            def save(path, value):
                if path == a.ready:
                    assert count[0] >= a.rate and value["samples"] == count[0]
                    ready.append(dict(samples=count[0], eof=delay.eof))
                original_json(path, value)
            with patch.object(worker, "write_iq", write), patch.object(worker, "write_json", save), \
                    contextlib.redirect_stderr(io.StringIO()):
                code = worker.stream(a, module, delay)
            report = json.loads(a.report.read_text())
            assert (code == 0) == (fault is None), (fault, report)
            assert ("cancel",) in events
            if fault == "post_tx":
                assert report.get("tx_readback_error") and report.get("tx_inhibit_error")
            else:
                assert report.get("tx_final", {}).get("lo_powerdown") == 1, (fault, report)
            if fault is None:
                expected.update(bytes(4096*4))  # existing decoder EOF lookahead, not capture
                assert got.digest() == expected.digest()
                assert count[0] == report["samples"]+4096
                assert report["delivered_samples"] == report["samples"]
                assert delay.eof == 1 and ready == [dict(samples=report["samples"], eof=1)]
            elif fault != "post_tx":
                assert delay.eof == 0 and not ready

    with tempfile.TemporaryDirectory() as tmp:
        a = args(Path(tmp), "--upsample", "2")
        module, events, _ = fake_iio()
        with contextlib.redirect_stderr(io.StringIO()):
            assert worker.stream(a, module, Delay()) == 1
        assert not any(event[0] == "context" for event in events)


if __name__ == "__main__":
    test_worker()
    test_interpolation()
    test_processor_delivery()
    test_backpressure()
    test_guards_and_plans()
    test_timing_and_rate_gate()
    test_payload_proof()
    test_coordinator()
    test_live_processes()
    if len(sys.argv) == 3 and sys.argv[1] == "--receiver":
        test_model_receiver(sys.argv[2])
    print("live libiio: TX inhibits, timing/backlog faults, interpolation, FIFO and exact TS proof pass; no devices opened")
