"""Public check rejects bad evidence, stays radio-off and continues after failures."""
import contextlib
import ctypes as ct
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("self_check", ROOT/"examples/self-check/check.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
sys.path.insert(0, str(ROOT/"host"))
import decode
import dvbs2
import numpy as np


def rejects(fn, *args):
    try:
        fn(*args)
    except (AssertionError, ValueError):
        return
    raise AssertionError("bad evidence accepted")


def test_decode():
    header = np.packbits(dvbs2.bbheader(dvbs2.matype1(dvbs2.GCS), 24)).tobytes()
    assert decode.parse_bbframes(header+b"abc")[0]["payload"] == b"abc"
    rejects(decode.parse_bbframes, header+b"ab")
    rejects(decode.parse_bbframes, bytes([header[0]^1])+header[1:]+b"abc")
    r = decode.summarize("FRAMELOCK 1\nLDPCCORR = 0\nBCHCORR = 0\n", [], b"")
    assert r["status_complete"] and r["ldpc_checks_before"] is None
    r.update(payload_match=True, payload_bit_errors=0, first_frame=0, payload_frames=14)
    check.check_decode(r, 11, 16)
    for key, value in (("ldpc_bit_flips", 1), ("bch_failed_frames", 1),
                       ("bch_corrected_bits", 1), ("payload_frames", 13),
                       ("status_complete", False), ("payload_match", False)):
        rejects(check.check_decode, dict(r, **{key: value}), 11, 16)
    assert decode.stream_match(b"abcdefabcdef", b"cdefab", 6) is None
    assert decode.stream_match(b"abcdefabcdef", b"abcdef", 6) == 0
    for mc in range(4,18):
        rate = dvbs2.RATES[mc-1] if mc < 12 else dvbs2.PSK8_RATES[mc-12]
        assert len(decode.payload(mc, 1, "N0CALL")) == (dvbs2.code(rate).kbch-80)//8


def row(mc, token):
    num, den, rate = check.mode(token)
    return dict(status="ESP_OK", encoder_failed=False, encoder_timed=True,
                modcod=mc, callsign="N0CALL", sps=num/den, symbol_rate=rate*den//num,
                rolloff=.20 if den == 5 else .35, frame_symbols=33282 if mc < 12 else 22194,
                pulse_symbols=7 if den == 5 else 6 if mc >= 12 and rate == 80000000 else 9,
                underruns=0, stall_ms=0, dac_rate=rate, seconds=10,
                late_fills=0, late_switches=0, fatal_late=0, min_fill_slack_cycles=1,
                frames_played=1, encoder_pie=15, frames_encoded=20, prefill_frames=3,
                encoder_stack_min_free=512, profile=dict(shaped_words=80, reused_words=20, fill_words=100),
                paced_wifi_up=True, wifi_free_up=22351, wifi_min_free=22351,
                queue_empty=0, queue_low_pages=1, positions=0, max_fill_call_cycles=0)


def test_pace_and_crc():
    r = row(11, "20m")
    check.check_pace([r], 11, "20m", 10)
    for k, v in (("late_fills", 1), ("underruns", 1), ("min_fill_slack_cycles", 0),
                 ("encoder_stack_min_free", 511), ("encoder_pie", 0)):
        rejects(check.check_pace, [dict(r, **{k: v})], 11, "20m", 10)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)/"dump.iq"
        data = bytes(16*r["frame_symbols"]*4*4)
        path.write_bytes(data)
        crc = zlib.crc32(data)
        r.update(frames=16, decimation=1, overlap_mismatches=0, crc32=crc)
        rows = [dict(binary_bytes=len(data), host_crc32=crc), r]
        check.check_dump(rows, path, "11/4/80/16", {"11/4/80/16": crc})
        rejects(check.check_dump, rows, path, "11/4/80/16", {"11/4/80/16": crc^1})
        path.write_bytes(data[:-4])
        rejects(check.check_dump, rows, path, "11/4/80/16", {"11/4/80/16": crc})


def test_keep_going():
    # --plan works without site-packages, pyserial, or any board driver import.
    subprocess.run([sys.executable, "-S", str(ROOT/"examples/self-check/check.py"), "--plan"],
                   check=True, stdout=subprocess.DEVNULL)
    sent = []
    revision = subprocess.check_output(["git", "describe", "--always", "--dirty"], cwd=ROOT, text=True).strip()
    def run(port, command, **kwargs):
        sent.append(command)
        if command == "info":
            assert kwargs["reset"]
            return [dict(firmware=revision, callsign="N0CALL")]
        if command.startswith("dvbdump"):
            raise RuntimeError("simulated USB dump failure")
        parts = command.split()
        mc, token = int(parts[1]), parts[2] if len(parts) == 4 or parts[0] == "wifimem" else ""
        r = row(mc, token)
        if parts[0] == "wifimem":
            r["frames_played"] = 0
        else:
            r["seconds"] = int(parts[-1])
        return [r]
    fake = types.SimpleNamespace(run=run, resolve_port=lambda p: p,
                                 reserve_port=lambda *args, **kw: contextlib.nullcontext())
    with tempfile.TemporaryDirectory() as tmp, patch.dict(sys.modules, board=fake), \
            patch.object(check.shutil, "which", return_value=sys.executable), \
            patch.object(check.subprocess, "run", return_value=types.SimpleNamespace(returncode=1, stdout="bad IQ", stderr="")), \
            patch.object(sys, "argv", ["check.py", str(Path(tmp)/"out")]), contextlib.redirect_stdout(io.StringIO()):
        assert check.main() == 1
        summary = (Path(tmp)/"out/summary.tsv").read_text()
        assert "PASS\tWi-Fi startup heap" in summary and "FAIL\tdump" in summary
    assert len(sent) == 1+len(check.DUMPS)+len(check.PACES)+1
    assert all(s.split()[0] in ("info", "dvbdump", "dvbpace", "wifimem") for s in sent)


def test_tones():
    # These CRCs came from the unmodified 88f5a64 generator, before removing
    # PRBS/sweep/gap probes. The retained tones must keep every sample.
    root = ROOT/"firmware/components/radio_s3"
    with tempfile.TemporaryDirectory() as tmp:
        so = Path(tmp)/"tones.so"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-I"+str(root/"include"), str(root/"radio_signals.c"), "-lm", "-o", str(so)], check=True)
        lib = ct.CDLL(str(so))
        lib.radio_signal_init.argtypes = [ct.c_void_p, ct.c_char_p]
        lib.radio_signal_init.restype = ct.c_bool
        lib.radio_signal_fill.argtypes = [ct.c_void_p, ct.c_uint64, ct.c_size_t, ct.c_void_p]
        golden = json.loads((ROOT/"tests/data/tone_crcs.json").read_text())
        for name, crc in golden.items():
            ctx = ct.create_string_buffer(164)  # bandwidth plus forty words
            assert lib.radio_signal_init(ctx, name.encode())
            guarded = (ct.c_uint32*4099)(*([0xa5a5a5a5]*4099))
            dst = ct.byref(guarded, 4)
            lib.radio_signal_fill(dst, 2**32+17, 4097, ctx)
            assert guarded[0] == guarded[-1] == 0xa5a5a5a5
            assert zlib.crc32(bytes(guarded)[4:-4]) == crc, name
            # Arbitrary callback splits preserve the same timeline.
            lib.radio_signal_fill(dst, 2**32+17, 39, ctx)
            lib.radio_signal_fill(ct.byref(guarded, 40*4), 2**32+56, 4058, ctx)
            assert zlib.crc32(bytes(guarded)[4:-4]) == crc, name
        for name in ("tag", "sweep", "gap", "tone0", "tone512", "unknown"):
            assert not lib.radio_signal_init(ctx, name.encode()), name


if __name__ == "__main__":
    test_decode()
    test_pace_and_crc()
    test_keep_going()
    test_tones()
    print("self-check: corrupt samples/status rejected, plan needs no devices, failures keep going; no TX commands")
