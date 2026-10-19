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
    r.update(payload_match=True, payload_bit_errors=0, first_frame=0, payload_frames=14,
             bb_frames=14)
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
    # Count accepted FEC frames by their decoded PLHEADER, not the requested mode.
    status = ''.join(f'S2FEC modcod={mc} short=0 pilots=1 ldpc_flips=0 '
                     f'ldpc_checks_before=0 ldpc_checks_after=0 bch_errors={bch}\n'
                     for mc, bch in ((4, 0), (7, 0), (4, 0), (13, -1)))
    assert decode.summarize(status, [], b'')['frames_per_modcod'] == {4: 2, 7: 1}


def test_dummy_decode():
    structures = json.loads((ROOT/"tests/data/psk8_26m667_frames.json").read_text())
    for mc, full, dummies in ((13,16,0),(15,14,8),(17,12,22)):
        before = structures[f"{mc}/3/80/16"]["dummy_before"]
        layout = decode.stream_layout(mc,16,before)
        assert layout["expected_payload_frames"] == full
        assert layout["expected_dummy_frames"] == dummies
    for before in ((3,2),(-1,),(17,)):
        rejects(decode.stream_layout,17,16,before)
    assert decode.stream_layout(17,1,(0,)*7) == dict(
        expected_payload_frames=0, expected_dummy_frames=6,
        partial_payload_symbols=0, partial_dummy_symbols=2214)
    # Empty BBFRAMEs carry no payload; dummy PLFRAMEs do not produce BBFRAMEs.
    size = len(decode.payload(17,1,"N0CALL"))
    expected = decode.payload(17,16,"N0CALL")
    got = expected[:11*size]
    frames = [dict(dfl_bits=size*8,stream_type=1) for _ in range(11)]
    frames += [dict(dfl_bits=0,stream_type=1)] * 2
    status = "FRAMELOCK 1\n" + "LDPCCORR = 0\nBCHCORR = 0\n" * 11
    report = decode.summarize(status,frames,got)
    assert report["payload_frames"] == 11 and report["bb_frames"] == 13
    before = structures["17/3/80/16"]["dummy_before"]
    with tempfile.TemporaryDirectory() as tmp:
        capture = Path(tmp)/"dump.cfile"
        capture.write_bytes(bytes(8))
        argv = ["decode.py",str(capture),"--format","cf32","--rate","80000000",
                "--symbol-rate",str(80000000/3),"--modcod","17","--stream","16",
                "--dummy-before",",".join(map(str,before))]
        def run(result, data):
            output = io.StringIO()
            with patch.object(sys,"argv",argv), patch.object(decode,"decode",return_value=(dict(result),data)), \
                    contextlib.redirect_stdout(output):
                code = decode.main()
            return code,json.loads(output.getvalue())
        code,r = run(report,got)
        assert code == 0 and r["payload_frames"] == 11 and r["expected_payload_frames"] == 12
        assert r["first_frame"] == 0 and r["compared_bits"] == 639232
        check.check_decode(r,17,16,12)
        # Preserve the same EOF allowance; one decoded frame cannot pass.
        rejects(check.check_decode,dict(r,payload_frames=1,bb_frames=3),17,16,12)
        rejects(check.check_decode,dict(r,payload_frames=8,bb_frames=10),17,16,12)
        rejects(check.check_decode,dict(r,expected_payload_frames=16),17,16,12)
        assert run(report,got[:40]+bytes([got[40]^1])+got[41:])[0] == 1
        assert run(dict(report,payload_frames=12),got)[0] == 1
        missing = expected[:4*size]+expected[5*size:12*size]
        assert run(report,missing)[0] == 1


def row(mc, token):
    num, den, rate = check.mode(token)
    return dict(status="ESP_OK", encoder_failed=False, encoder_timed=True,
                modcod=mc, callsign="N0CALL", sps=num/den, symbol_rate=rate*den//num,
                rolloff=.20 if den == 5 else .35, frame_symbols=33282 if mc < 12 else 22194,
                pulse_symbols=7 if den == 5 else 5 if mc >= 12 and num == 3 else
                              6 if mc >= 12 and rate == 80000000 else 9,
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


def test_8psk_gates():
    records = ROOT/"docs/measurements"
    pace = [json.loads(line) for line in
            (records/"2026-10-18-8psk-26m667-pace.jsonl").read_text().splitlines()]
    assert {r["modcod"] for r in pace} == {13, 15, 17}
    for r in pace:
        mc = r["modcod"]
        keyed = next(json.loads(line) for line in
            (records/"2026-10-18-8psk-26m667-keyed.jsonl").read_text().splitlines()
            if json.loads(line)["modcod"] == mc)
        assert check.PSK8_KEYED[mc] == tuple(keyed[k] for k in
            ("max_fill_cycles", "encode_cycles_mean", "encode_cycles_live_max"))
        check.check_pace([r], mc, "26m667", 20)
        fill, mean, live = check.PSK8_KEYED[mc]
        for key, value in (("max_fill_cycles", fill + check.FILL_OVERHEAD + 1),
                           ("encode_cycles_mean", mean + check.ENCODER_OVERHEAD + 1),
                           ("encode_cycles_live_max", live + check.ENCODER_OVERHEAD + 1),
                           ("dummy_steady", 20 * check.PSK8_DUMMIES_PER_SECOND[mc] + 1),
                           ("dummy_startup", 5), ("queue_stream_bound_misses", 2),
                           ("frames_played", 10), ("queue_pages", 50),
                           ("wifi_min_free", 19999), ("queue_empty", 1),
                           ("queue_sizing", dict(automatic=True, ready=False))):
            rejects(check.check_pace, [dict(r, **{key: value})], mc, "26m667", 20)
        # Ten-second self-check runs scale dummy and data limits, not timing ceilings.
        short = dict(r, seconds=10, dummy_steady=r["dummy_steady"]//2,
                     frames_played=r["frames_played"]//2)
        check.check_pace([short], mc, "26m667", 10)
        rejects(check.check_pace, [dict(short, dummy_steady=10*check.PSK8_DUMMIES_PER_SECOND[mc]+1)],
                mc, "26m667", 10)
    assert "17/3/80/16" in check.DUMPS
    assert all((mc, "26m667", 10) in check.PACES for mc in (13, 15, 17))


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
        if command.startswith("vcm"):
            return [dict(cmd="vcm", axes=False, enabled=False, pattern=[4, 11, 13, 17], hold_frames=1)]
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
    assert len(sent) == 1+len(check.DUMPS)+len(check.PACES)+1+5
    assert all(s.split()[0] in ("info", "vcm", "dvbdump", "dvbpace", "wifimem") for s in sent)


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
            ctx = ct.create_string_buffer(8+4001*4)  # bandwidth, period and waveform scratch
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
        assert lib.radio_signal_init(ctx, b"chirpwide")
        period = (ct.c_uint32*4001)()
        lib.radio_signal_fill(period, 0, 4001, ctx)
        lib.radio_signal_fill(dst, 2**32+17, 4097, ctx)
        assert guarded[0] == guarded[-1] == 0xa5a5a5a5
        assert list(guarded)[1:-1] == [period[(2**32+17+n) % 4001] for n in range(4097)]
        for name in ("tag", "sweep", "gap", "tone0", "tone512", "unknown"):
            assert not lib.radio_signal_init(ctx, name.encode()), name


if __name__ == "__main__":
    test_decode()
    test_dummy_decode()
    test_pace_and_crc()
    test_8psk_gates()
    test_keep_going()
    test_tones()
    print("self-check: corrupt samples/status rejected, plan needs no devices, failures keep going; no TX commands")
