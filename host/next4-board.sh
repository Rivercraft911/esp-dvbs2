#!/bin/bash
# Run from a clean exp/next4 checkout after the coordinator's single flash.
# About 10 minutes. Radio off: dumps, paced simulation and Wi-Fi start/stop only.
# Pass criteria are printed BEFORE each command. See docs/next4.md for their sources.
set -euo pipefail
cd "$(dirname "$0")/.."
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
OUT=${1:-notes/next4-board-$(date +%Y%m%d-%H%M%S)}
FW=$(git describe --always --dirty)
mkdir -p "$(dirname "$OUT")"
mkdir "$OUT"

board() {
    "$PY" host/board.py "$@" --timeout 90 --out "$OUT/runs.jsonl"
}

check() {
    "$PY" - "$OUT" "$FW" "$@" <<'PY'
import json, pathlib, sys, zlib
folder, firmware, kind, command, *limits = sys.argv[1:]
folder = pathlib.Path(folder)
rows = [json.loads(line) for line in (folder/'runs.jsonl').read_text().splitlines()]
rows = [r for r in rows if r.get('command') == command and not r.get('done')]
assert rows and not any('error' in r for r in rows), rows
if kind == 'info':
    assert len(rows) == 1 and rows[0]['firmware'] == firmware, rows
    assert rows[0]['callsign'] == 'N0CALL', 'CRCs require stored N0CALL; stop, do not change NVS'
else:
    result = [r for r in rows if 'status' in r]
    assert len(result) == 1, rows
    r = result[0]
    assert r['status'] == 'ESP_OK' and not r['encoder_failed'], r
    assert r['callsign'] == 'N0CALL' and r['underruns'] == 0, r
    sps = r['dac_rate'] // r['symbol_rate'] if 'dac_rate' in r else (
        40 if r['modcod'] == 4 else 8 if command.split()[2] in ('5m', '10m80') else 4)
    assert r['packed_mapper'] == ('none' if sps == 40 else 'direct' if sps == 4 else 'sparse'), r
    assert r['workspace_memory'] == ('internal' if sps == 4 else 'psram'), r
    assert r['ring_slots'] == (8 if sps == 40 else 3 if sps == 4 else 2), r
    if '20ms6' in command:
        assert r['pulse_symbols'] == 6 and r['ring_heap_bytes'] == 0, r
    else:
        assert r['pulse_symbols'] == 9, r
    if kind == 'dump':
        crc, filename = limits
        data = (folder/filename).read_bytes()
        binary = [b for b in rows if 'binary_bytes' in b]
        assert len(binary) == 1 and len(data) == binary[0]['binary_bytes'], binary
        assert r['crc32'] == zlib.crc32(data) == int(crc), r
        assert r['overlap_mismatches'] == 0 and r['frames'] == 40, r
    elif kind == 'pace':
        fill, mean, live, slack = map(int, limits)
        assert r['seconds'] == 30 and r['stall_ms'] == 0, r
        assert all(r[k] == 0 for k in ('late_fills', 'late_switches', 'fatal_late')), r
        assert r['max_fill_cycles'] <= fill and r['min_fill_slack_cycles'] >= slack, r
        assert r['encode_cycles_mean'] <= mean and r['encode_cycles_live_max'] <= live, r
        p = r['profile']
        assert p['shaped_words'] + p['reused_words'] == p['fill_words'], r
    elif kind == 'wifi':
        assert r['wifi_free_up'] >= 20000 and r['wifi_min_free'] >= 20000, r
    else:
        raise AssertionError(kind)
print('PASS:', command)
PY
}

printf 'PASS first: firmware %s, stored N0CALL. No NVS writes.\n' "$FW"
board info --reset
check info info

printf 'PASS first: fecbench pie, all 140 reference CRCs and 14 PIE checks, zero mismatches.\n'
"$PY" host/board.py 'fecbench pie' --timeout 120 --out "$OUT/fec.jsonl"
"$PY" tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only

# Every dump: ESP_OK, no encoder failure/underrun, zero overlap mismatches,
# 40 frames, complete saved binary and both firmware/host CRC32 equal this value.
while IFS='|' read -r args crc file; do
    printf 'PASS first: dvbdump %s; saved CRC32 %s, zero overlap errors.\n' "$args" "$crc"
    board "dvbdump $args" --save "$OUT/$file"
    check dump "dvbdump $args" "$crc" "$file"
done <<'DUMPS'
4 40|1435641601|4.iq
13 5m 40 128 1|1937985440|13-5m.iq
15 5m 40 128 1|1163322913|15-5m.iq
13 10m 40 128 1|474263962|13-10m.iq
15 10m 40 128 1|1757765864|15-10m.iq
13 20m 40 128 1|474263962|13-20m.iq
15 20m 40 128 1|1757765864|15-20m.iq
13 10m80 40 128 1|1937985440|13-10m80.iq
13 20ms6 40 128 1|3178964763|13-20ms6.iq
15 20ms6 40 128 1|4256019779|15-20ms6.iq
DUMPS

# Every pace: ESP_OK, zero encoder failures, underruns, late fills/switches and
# fatal lateness, correct mapper/workspace/ring, conserved shaped + reused words.
# Columns: command | fill ceiling | encoder mean ceiling | live ceiling | slack floor.
# All cycle limits are cycles, encoder limits are cycles/frame. Startup is retained
# in encode_cycles_max; live excludes only the firmware's reported ring prefill.
while IFS='|' read -r args fill mean live slack; do
    printf 'PASS first: dvbpace %s; zero late/underrun; fill <=%s, mean <=%s, live <=%s, slack >=%s cycles.\n' \
        "$args" "$fill" "$mean" "$live" "$slack"
    board "dvbpace $args"
    check pace "dvbpace $args" "$fill" "$mean" "$live" "$slack"
done <<'PACES'
13 5m 30|45200|227257|234116|1
15 5m 30|45200|260161|265713|1
13 10m 30|60000|170493|172099|1
13 20m 30|46249|170371|173506|700
13 20ms6 30|41276|172482|180589|5671
15 20ms6 30|41278|190932|194981|5671
4 30|43457|695717|701401|1
PACES

# Wi-Fi NULL-mode start/stop only. No samples played, TX keying or retuning.
for args in '13 20ms6' '17 5m'; do
    printf 'PASS first: wifimem %s; ESP_OK, encoder healthy, Wi-Fi-up and minimum internal heap >=20000 B.\n' "$args"
    board "wifimem $args"
    check wifi "wifimem $args"
done
printf 'All radio-off checks passed. Records and ten IQ dumps: %s\n' "$OUT"
