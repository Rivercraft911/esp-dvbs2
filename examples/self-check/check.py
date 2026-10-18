#!/usr/bin/env python3
"""Radio-off board regression: dumps, independent decode and timed bank fills.

--plan prints fixed criteria without importing the board driver or opening devices.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
DUMPS = ("4/40/40/16", "13/8/40/16", "11/4/80/16",
         "15/4/80/16", "11/3/80/16", "11/12/80/16", "17/3/80/16")
PACES = ((4, "", 5), (11, "5m", 5), (11, "10m", 5),
         (11, "20m", 5), (11, "26m667", 5), (11, "33m333", 10),
         (17, "5m", 5), (13, "10m", 5), (15, "20m", 5),
         (13, "26m667", 10), (15, "26m667", 10), (17, "26m667", 10))

# eee5153 keyed records: longest bank fill, encoder mean and live maximum.
# Wi-Fi-up pace records add <=3894 fill cycles and <=7023 encoder mean cycles.
# Fixed allowances stay below a bank deadline / frame duration; no 15% claim.
PSK8_KEYED = {13: (39419, 172076, 239463),
              15: (38803, 176477, 251019),
              17: (38963, 158719, 213647)}
FILL_OVERHEAD = 4500
ENCODER_OVERHEAD = 10000
# Steady dummies/s observed over 20 s: 173.6, 213.1, 5.0. Bound both their
# count and the resulting data throughput; losing arbitrary airtime must fail.
PSK8_DUMMIES_PER_SECOND = {13: 180, 15: 220, 17: 6}


def mode(token):
    num, den = {"": (40, 1), "5m": (8, 1), "10m": (4, 1),
                "20m": (4, 1), "26m667": (3, 1), "33m333": (12, 5)}[token]
    rate = 80000000 if token in ("20m", "26m667", "33m333") else 40000000
    return num, den, rate


def dump_args(key):
    mc, num, mhz, frames = map(int, key.split("/"))
    token = ("33m333" if num == 12 else "26m667" if num == 3 else
             "20m" if mhz == 80 else {40: "", 8: "5m", 4: "10m"}[num])
    decim = 10 if num == 40 else 2 if mc < 12 and num == 8 else 1
    return mc, token, frames, decim


def stream(rows, mc, token):
    results = [r for r in rows if "status" in r]
    assert len(results) == 1, rows
    r = results[0]
    num, den, rate = mode(token)
    assert r["status"] == "ESP_OK" and not r["encoder_failed"], r
    assert r["encoder_timed"] and r["modcod"] == mc and r["callsign"] == "N0CALL", r
    assert r["sps"] == num / den and r["symbol_rate"] == rate * den // num, r
    assert r["rolloff"] == (.20 if den == 5 else .35), r
    assert r["pulse_symbols"] == (7 if den == 5 else
                                  5 if mc >= 12 and num == 3 else
                                  6 if mc >= 12 and rate == 80000000 else 9), r
    assert r["underruns"] == r["stall_ms"] == 0, r
    return r


def check_dump(rows, path, key, golden):
    mc, token, frames, decim = dump_args(key)
    r = stream(rows, mc, token)
    binary = [b for b in rows if "binary_bytes" in b]
    num, den, _ = mode(token)
    samples = (frames * r["frame_symbols"] * num + den - 1) // den
    data = path.read_bytes()
    assert len(binary) == 1 and len(data) == binary[0]["binary_bytes"] == (samples+decim-1)//decim*4, r
    assert r["frames"] == frames and r["decimation"] == decim and r["overlap_mismatches"] == 0, r
    assert r["crc32"] == binary[0]["host_crc32"] == zlib.crc32(data) == golden[key], r


def check_decode(r, mc, frames):
    assert r["frame_lock"] and r["status_complete"] and r["payload_match"], r
    assert 0 <= r["first_frame"] <= 1, r
    assert (r["payload_frames"] + r["first_frame"] >= frames-3 if mc >= 12 else
            r["payload_frames"] >= frames-2), r
    for k in ("bch_failed_frames", "bch_corrected_bits", "ldpc_bit_flips", "payload_bit_errors",
              "bbheader_rejected_frames"):
        assert r[k] == 0, r
    # Stock LeanDVB does not report syndrome weights. Never turn missing data into zero.
    for k in ("ldpc_checks_before", "ldpc_checks_after"):
        assert r[k] is None or r[k] == 0, r


def check_pace(rows, mc, token, seconds):
    r = stream(rows, mc, token)
    num, den, rate = mode(token)
    assert r["seconds"] == seconds and r["dac_rate"] == rate, r
    assert all(r[k] == 0 for k in ("late_fills", "late_switches", "fatal_late")), r
    assert r["min_fill_slack_cycles"] > 0 and r["frames_played"] > 0, r
    assert r["encoder_pie"] == 15 and r["frames_encoded"] > r["prefill_frames"], r
    assert r["encoder_stack_min_free"] >= 512, r
    profile = r["profile"]
    assert profile["shaped_words"] + profile["reused_words"] == profile["fill_words"], r
    if den == 5 or (mc >= 12 and num == 3):
        assert r["paced_wifi_up"] and r["wifi_free_up"] >= 20000 and r["wifi_min_free"] >= 20000, r
        assert r["queue_empty"] == 0 and r["queue_low_pages"] > 0, r
    if mc >= 12 and num == 3:
        fill, mean, live = PSK8_KEYED[mc]
        assert 0 < r["max_fill_cycles"] <= fill + FILL_OVERHEAD < 47220, r
        assert 0 < r["encode_cycles_mean"] <= mean + ENCODER_OVERHEAD < 199746, r
        assert 0 < r["encode_cycles_live_max"] <= live + ENCODER_OVERHEAD, r
        assert r["queue_pages"] == 54 and r["queue_sizing"]["automatic"] and r["queue_sizing"]["ready"], r
        assert r["queue_stream_bound_misses"] <= 1, r  # one observed cold calibration miss
        dummy_limit = PSK8_DUMMIES_PER_SECOND[mc] * seconds
        assert 0 <= r["dummy_startup"] <= 4 and 0 <= r["dummy_steady"] <= dummy_limit, r
        # Subtract the entire allowed dummy airtime, plus one partial data frame.
        minimum = (seconds * rate // num - (dummy_limit + 4) * 3330) // 22194 - 1
        assert r["frames_played"] >= minimum, r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("out", nargs="?", type=Path)
    p.add_argument("--plan", action="store_true")
    p.add_argument("--port", default="/dev/cu.usbmodem101")
    p.add_argument("--receiver", default=os.environ.get("LEANDVB", "leandvb"))
    a = p.parse_args()
    golden = {}
    for name in ("waveform_crcs.json", "moon_stream_crcs.json", "psk8_26m667_crcs.json"):
        golden.update(json.loads((ROOT/"tests/data"/name).read_text()))
    revision = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
    if subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=ROOT).returncode:
        revision += "-dirty"
    print(f"Pass criteria: flashed firmware {revision}, stored N0CALL, SPI input off after reset.")
    print("No flash, NVS writes or TX. Only info, dvbdump, dvbpace and wifimem are sent.")
    print("Every stream: ESP_OK, correct mode/pulse, healthy timed encoder, no underruns or injected stalls.")
    print("Dumps: exact byte count, zero overlap errors, firmware/USB/file/golden CRC32 all equal.")
    for key in DUMPS:
        print(f"  {key}: CRC32 {golden[key]}")
    print("LeanDVB: frame lock, complete FEC diagnostics, contiguous exact payload; no LDPC flips,")
    print("  BCH errors/corrections or bad BB headers. Reported syndrome counts must be zero.")
    print("  QPSK >=N-2 frames; 8PSK <=1 leading and <=3 trailing acquisition/EOF frames.")
    print("Paces: zero late fills/switches/fatal lateness, positive slack, PIE mask 15, stack >=512 B,")
    print("  live encoder and conserved fill words. 33m333: queue never empty, Wi-Fi free/min >=20000 B.")
    for mc, token, seconds in PACES:
        command = f"dvbpace {mc} {token} {seconds}".replace("  ", " ")
        print(f"  {command}" + ("  # 1 Msym/s" if not token else ""))
    print("8PSK 26m667: 54 ready pages; no empty queue; Wi-Fi free/min >=20000 B;")
    print("  <=4 startup dummies, <=1 cold bound miss; bounded steady dummies and data throughput.")
    for mc, (fill, mean, live) in PSK8_KEYED.items():
        dummy_limit = 10 * PSK8_DUMMIES_PER_SECOND[mc]
        minimum = (10 * 80000000 // 3 - (dummy_limit + 4) * 3330) // 22194 - 1
        print(f"  {mc}: fill <= {fill+FILL_OVERHEAD}, encoder mean/live <= "
              f"{mean+ENCODER_OVERHEAD}/{live+ENCODER_OVERHEAD} cycles; "
              f"steady dummies <= {PSK8_DUMMIES_PER_SECOND[mc]}/s; "
              f">={minimum} data frames in 10 s.")
    print("8PSK ceilings: eee5153 keyed +4500 fill / +10000 encoder cycles for profiling;")
    print("  pace dummies are measured overhead, not evidence of full-rate keyed throughput.")
    print("Other modes enforce functional deadlines. These are not 15% reserve gates.")
    print("40 MHz met 15% reserve on both cores in one 30 s keyed run (e96795b); see docs/measurements.md.")
    print("wifimem 17 5m: Wi-Fi startup succeeds; free/up and command minimum both >=22351 B.")
    print("Keep going after failures; save IQ, logs, revision and summary; any failure exits 1.", flush=True)
    if a.plan:
        return 0

    # Imports and receiver discovery precede device access; --plan needs no board dependencies.
    sys.path.insert(0, str(ROOT/"host"))
    import board
    import numpy  # noqa: F401
    import scipy  # noqa: F401
    if not shutil.which(a.receiver):
        p.error("LeanDVB not found; see examples/self-check/README.md")
    out = (a.out or ROOT/"notes"/dt.datetime.now().strftime("self-check-%Y%m%d-%H%M%S")).resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out/"revision.json").write_text(json.dumps(dict(firmware=revision,
        commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        receiver=str(Path(shutil.which(a.receiver)).resolve()),
        started=dt.datetime.now(dt.timezone.utc).isoformat()), indent=2)+"\n")
    outcomes = []

    def step(label, fn):
        try:
            fn()
            verdict, detail = "PASS", ""
        except Exception as exc:
            verdict, detail = "FAIL", str(exc)[:500]
        outcomes.append((verdict, label, detail))
        with (out/"summary.tsv").open("a") as f:
            f.write(f"{verdict}\t{label}\t{detail}\n")
        print(f"{verdict}: {label}" + (f" {detail}" if detail else ""), flush=True)

    def command(text, save=None, reset=False):
        assert text.split()[0] in ("info", "dvbdump", "dvbpace", "wifimem"), text
        rows = board.run(a.port, text, timeout=120, reset=reset, save=save)
        with (out/"runs.jsonl").open("a") as f:
            for row in rows:
                f.write(json.dumps(dict(command=text, **row))+"\n")
        assert rows and not any("error" in r for r in rows), rows
        return rows

    def info():
        rows = command("info", reset=True)
        assert len(rows) == 1 and rows[0]["firmware"] == revision and rows[0]["callsign"] == "N0CALL", rows

    def dump(key):
        mc, token, frames, decim = dump_args(key)
        path = out/(key.replace("/", "-")+".iq")
        rows = command(f"dvbdump {mc} {token} {frames} 128 {decim}".replace("  ", " "), save=path)
        check_dump(rows, path, key, golden)

    def memory():
        r = stream(command("wifimem 17 5m"), 17, "5m")
        assert r["wifi_free_up"] >= 22351 and r["wifi_min_free"] >= 22351, r
        assert r["frames_played"] == r["positions"] == r["max_fill_call_cycles"] == 0, r

    def decode(key):
        mc, token, frames, decim = dump_args(key)
        _, _, rate = mode(token)
        num, den, _ = mode(token)
        name = key.replace("/", "-")
        cmd = [sys.executable, str(ROOT/"host/decode.py"), str(out/(name+".iq")),
               "--rate", str(rate//decim), "--symbol-rate", str(rate*den/num),
               "--rolloff", str(.20 if den == 5 else .35), "--modcod", str(mc),
               "--stream", str(frames), "--receiver", a.receiver, "--log", str(out/(name+".receiver.log"))]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=150)
        (out/(name+".decode.json")).write_text(result.stdout)
        (out/(name+".decode.stderr")).write_text(result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        check_decode(json.loads(result.stdout), mc, frames)

    a.port = board.resolve_port(a.port)
    try:
        lock = board.reserve_port(a.port, reset=True)
    except Exception as exc:
        step("serial ownership", lambda: (_ for _ in ()).throw(exc))
        return 1
    with lock:
        step("info", info)
        for key in DUMPS:
            step("dump "+key, lambda key=key: dump(key))
            step("decode "+key, lambda key=key: decode(key))
        for mc, token, seconds in PACES:
            text = f"dvbpace {mc} {token} {seconds}".replace("  ", " ")
            step(text, lambda mc=mc, token=token, seconds=seconds, text=text:
                 check_pace(command(text), mc, token, seconds))
        step("Wi-Fi startup heap", memory)
    failed = sum(row[0] == "FAIL" for row in outcomes)
    print(f"{len(outcomes)-failed}/{len(outcomes)} passed. Records: {out}")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
