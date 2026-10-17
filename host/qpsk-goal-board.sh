#!/bin/bash
# Radio-off integration gates after the coordinator's build/flash. This script never flashes.
# Radio off: FEC, USB dumps, paced simulation and Wi-Fi NULL-mode start/stop only.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
OUT=${1:-notes/qpsk-goal-board-$(date +%Y%m%d-%H%M%S)}
RX=${RX:-/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb}
FW=$(git describe --always --dirty) || exit 1
mkdir -p "$(dirname "$OUT")" && mkdir "$OUT" || exit 1
exec > >(tee "$OUT/console.log") 2>&1
passed=0
failed=0
printf 'command\tboard_rc\tcheck_rc\tresult\n' > "$OUT/summary.tsv"

cat <<CRITERIA
Pass criteria, fixed before running (docs/qpsk-goal.md):
  info: firmware $FW and stored N0CALL. No NVS writes.
  fecbench pie: all 140 reference CRCs, 14 PIE checks, zero mismatches.
  All 22 dumps: ESP_OK, healthy encoder, zero underruns/overlap mismatches,
    requested frames, exact saved byte count, firmware/USB/file CRC32 equal the listed CRC.
  All six paces: 30 s, no stall, ESP_OK, zero underruns, late fills, late switches
    or fatal lateness; positive slack; fill/encoder ceilings printed per command.
  All stream results: expected mapper, workspace, BCH, ring and table-store layout.
  New paces: full encoder running, fill <=40137 cycles, slack >=7083 cycles;
    encoder mean/live max <=254607 cycles at 26m667. No pre-encoded input.
  Every dump: LeanDVB recovers >=frames-2 contiguous bit-exact payload frames,
    zero LDPC checks/flips, BCH failures/corrections and payload errors.
    New-mode raw-sample MER >=37 dB (3 sps), >=40 dB (4 sps).
  Wi-Fi: ESP_OK start/stop only, no played samples; free-up AND low-water internal
    heap >=30000 B for 11/8 26m667 and >=20000 B for 17 5m / 8 20mq4.
Every command and checker runs even after failure. Exit 1 if any check fails.
All output, every command's return codes and saved IQ: $OUT
CRITERIA

board() {
    "$PY" host/board.py "$@" --timeout 120 --out "$OUT/runs.jsonl"
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
    assert rows[0]['callsign'] == 'N0CALL', 'CRCs require stored N0CALL; do not change NVS'
    print('PASS: info, firmware', firmware, 'stored N0CALL')
    sys.exit(0)
result = [r for r in rows if 'status' in r]
assert len(result) == 1, rows
r = result[0]
assert r['status'] == 'ESP_OK' and not r['encoder_failed'], r
assert r['callsign'] == 'N0CALL' and r['underruns'] == 0 and r['stall_ms'] == 0, r
mc = r['modcod']
token = command.split()[2]
sps = 8 if token in ('5m', '10m80') else 3 if token == '26m667' else 4 if token in ('10m', '20m', '20ms6', '20mq4') else 40
rate = 80000000 if token in ('10m80', '20m', '20ms6', '26m667', '20mq4') else 40000000
quads = token in ('26m667','20mq4')
resident = sps != 40 and (mc < 12 or sps == 4)
assert r['encoder_timed'] is True and r['sps'] == sps, r
assert r['symbol_rate'] == rate // sps, r
assert r['packed_mapper'] == ('none' if sps == 40 else 'quads' if quads else 'direct' if resident else 'sparse'), r
assert r['workspace_memory'] == ('internal' if resident else 'psram'), r
assert r['workspace_bytes'] == 12420, r
assert r['workspace_heap_bytes'] == 0 if quads and sps == 4 else r['workspace_heap_bytes'] >= 12420, r
assert r['fast_heap_bytes'] >= r['fast_bytes'], r
cached = resident and mc not in (10, 11, 16, 17)
assert r['bch_cache_bytes'] == (8192 if cached else 0), r
assert r['bch_variant'] == ('resident-byte' if cached else 'nibble'), r
assert r['ring_slots'] == (8 if sps == 40 else 3 if sps <= 4 else 2), r
assert r['ring_format'] == ('raw' if sps == 40 else 'quads' if quads else 'packed'), r
assert r['ring_memory'] == r['payload_memory'] == ('psram' if sps == 40 else 'internal'), r
short = token == '20ms6'
assert r['pulse_symbols'] == (6 if short else 9), r
if quads:
    layout = r['layout']
    table, fast, workspace, payload, filler = (int(layout[k],16) for k in ('table','fast','workspace','payload','filler'))
    slots = [int(a,16) for a in layout['slots']]
    assert all(a % 16 == 0 for a in (table,fast,workspace,*slots)), r
    stored = 3 if sps == 3 else 0
    assert slots[:stored] == [table+12288*sps+i*8352 for i in range(stored)], r
    assert filler == table+12288*sps+(stored*8352 if sps == 3 else 12432) and filler+864 <= table+65536, r
    assert fast <= payload < payload+r['payload_bytes_per_frame'] <= fast+r['fast_bytes'], r
    assert r['ring_heap_bytes'] == 0 if sps == 3 else r['ring_heap_bytes'] >= 25056, r
    assert len(set(slots)) == 3, r
    if sps == 4: assert workspace == table+49152, r
elif sps != 40:
    layout = r['layout']
    table, payload, filler = (int(layout[k], 16) for k in ('table', 'payload', 'filler'))
    slots = [int(a, 16) for a in layout['slots']]
    frame = (r['frame_symbols'] // 2 + 31) // 16 * 16
    dummy = (3330 // 2 + 31) // 16 * 16
    assert len(slots) == len(set(slots)) == r['ring_slots'], r
    assert all(a % 16 == 0 for a in slots), r
    assert slots[1] == slots[0] + frame and payload == filler + dummy, r
    end = payload + r['payload_bytes_per_frame']
    if mc < 12:
        assert slots[0] == table + (4096 if sps == 4 else 8192), r
        assert slots == [slots[0] + i * frame for i in range(r['ring_slots'])], r
        assert filler == slots[-1] + frame and end <= table + 65536, r
        assert r['ring_heap_bytes'] == 0, r
    elif short:
        assert slots[0] == table + 16384 and filler == slots[0] + 2 * frame, r
        assert slots[2] >= end and slots[2] + frame <= table + 65536, r
        assert r['ring_heap_bytes'] == 0, r
    elif sps == 4:
        assert slots[0] == table + 32768 and filler == slots[0] + 2 * frame, r
        assert end <= table + 65536 and not table <= slots[2] < table + 65536, r
        assert r['ring_heap_bytes'] >= frame, r
    else:
        assert filler == slots[0] + 2 * frame and r['ring_heap_bytes'] >= end - slots[0], r
if kind == 'dump':
    crc, filename = limits
    data = (folder/filename).read_bytes()
    binary = [b for b in rows if 'binary_bytes' in b]
    assert len(binary) == 1, binary
    frames = 16 if quads else 40
    assert r['frames'] == frames and r['overlap_mismatches'] == 0, r
    decim = 10 if sps == 40 else 2 if mc < 12 and sps == 8 else 1
    assert r['decimation'] == decim and r['samples'] == frames * r['frame_symbols'] * sps, r
    assert len(data) == binary[0]['binary_bytes'] == r['samples'] // decim * 4, binary
    assert r['crc32'] == binary[0]['host_crc32'] == zlib.crc32(data) == int(crc), r
    detail = f"CRC32 {r['crc32']}, {len(data)} B"
elif kind == 'pace':
    fill, mean, live, slack = map(int, limits)
    assert r['seconds'] == 30 and r['dac_rate'] == rate, r
    assert all(r[k] == 0 for k in ('late_fills', 'late_switches', 'fatal_late')), r
    assert r['max_fill_cycles'] <= fill and r['min_fill_slack_cycles'] >= slack, r
    assert r['encode_cycles_mean'] <= mean and r['encode_cycles_live_max'] <= live, r
    assert r['prefill_frames'] == r['ring_slots'] and r['frames_played'] > 0, r
    p = r['profile']
    assert p['shaped_words'] + p['reused_words'] == p['fill_words'], r
    detail = (f"fill {r['max_fill_cycles']}, slack {r['min_fill_slack_cycles']} cycles; "
              f"encoder {r['encode_cycles_mean']}/{r['encode_cycles_live_max']} cycles/frame")
elif kind == 'wifi':
    floor = int(limits[0])
    assert r['wifi_free_up'] >= floor and r['wifi_min_free'] >= floor, r
    assert r['frames_played'] == 0 and r['positions'] == 0 and r['max_fill_call_cycles'] == 0, r
    detail = f"Wi-Fi up/min {r['wifi_free_up']}/{r['wifi_min_free']} B"
else:
    raise AssertionError(kind)
print('PASS:', command, detail)
PY
}

report() {
    local command=$1 board_rc=$2 check_rc=$3 verdict
    if (( board_rc == 0 && check_rc == 0 )); then
        verdict=PASS
        passed=$((passed + 1))
    else
        verdict=FAIL
        failed=$((failed + 1))
    fi
    printf '%s\t%s\t%s\t%s\n' "$command" "$board_rc" "$check_rc" "$verdict" >> "$OUT/summary.tsv"
    printf '%s: %s (command rc=%s, checker rc=%s)\n' "$verdict" "$command" "$board_rc" "$check_rc"
}

decode_dump() {
    local args=$1 file=$2 mc token frames peak decim sample_rate symbol_rate sps mer=0
    read -r mc token frames peak decim <<< "$args"
    case "$token" in
        5m) sps=8; sample_rate=40000000; symbol_rate=5000000;;
        10m) sps=4; sample_rate=40000000; symbol_rate=10000000;;
        10m80) sps=8; sample_rate=80000000; symbol_rate=10000000;;
        20m|20ms6|20mq4) sps=4; sample_rate=80000000; symbol_rate=20000000;;
        26m667) sps=3; sample_rate=80000000; symbol_rate=26666666.666666668;;
        *) frames=$token; sps=40; sample_rate=40000000; symbol_rate=1000000;;
    esac
    if [[ -z "${decim:-}" ]]; then
        if (( sps == 40 )); then decim=10
        elif (( mc < 12 && sps == 8 )); then decim=2
        else decim=1; fi
    fi
    local extra=(--shift 0)
    if [[ "$token" == 26m667 || "$token" == 20mq4 ]]; then
        extra+=(--mer); mer=40
        [[ "$token" == 26m667 ]] && mer=37
    fi
    "$PY" host/onair.py "$OUT/$file" --rate "$((sample_rate / decim))" --symbol-rate "$symbol_rate" \
        --scale 512 --stream "$frames" --modcod "$mc" --callsign N0CALL \
        --receiver "$RX" "${extra[@]}" > "$OUT/$file.decode.json"
    local rc=$?
    "$PY" - "$OUT/$file.decode.json" "$frames" "$mer" <<'PYDECODE'
import json, pathlib, sys
r = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert r['payload_match'] and r['payload_frames'] >= int(sys.argv[2])-2, r
assert all(r[k] == 0 for k in ('bch_failed_frames','bch_corrected_bits',
                              'ldpc_checks_before','ldpc_bit_flips','payload_bit_errors')), r
if int(sys.argv[3]): assert r['mer']['mer_db'] >= int(sys.argv[3]), r
print('PASS: LeanDVB', r['payload_frames'], 'contiguous bit-exact frames')
PYDECODE
    report "decode $file" "$rc" "$?"
}

printf '\nPASS first: info; firmware %s, stored N0CALL.\n' "$FW"
board info --reset
rc=$?
check info info
report info "$rc" "$?"

printf '\nPASS first: fecbench pie; 140 reference CRCs, 14 zero-mismatch PIE checks.\n'
"$PY" host/board.py 'fecbench pie' --timeout 120 --out "$OUT/fec.jsonl"
rc=$?
"$PY" tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only
report 'fecbench pie' "$rc" "$?"

while IFS='|' read -r args crc file; do
    printf '\nPASS first: dvbdump %s; saved CRC32 %s, requested frames, no overlap errors; LeanDVB bit-exact.\n' "$args" "$crc"
    board "dvbdump $args" --save "$OUT/$file"
    rc=$?
    check dump "dvbdump $args" "$crc" "$file"
    report "dvbdump $args" "$rc" "$?"
    decode_dump "$args" "$file"
done <<'DUMPS'
4 40|1435641601|4-1m.iq
13 5m 40 128 1|1937985440|13-5m.iq
15 5m 40 128 1|1163322913|15-5m.iq
13 10m 40 128 1|474263962|13-10m.iq
15 10m 40 128 1|1757765864|15-10m.iq
13 20m 40 128 1|474263962|13-20m.iq
15 20m 40 128 1|1757765864|15-20m.iq
13 10m80 40 128 1|1937985440|13-10m80.iq
13 20ms6 40 128 1|3178964763|13-20ms6.iq
15 20ms6 40 128 1|4256019779|15-20ms6.iq
4 5m 40|3933043456|4-5m.iq
11 5m 40|1552954365|11-5m.iq
4 10m 40|2594131925|4-10m.iq
11 10m 40|2779934507|11-10m.iq
4 10m80 40|3933043456|4-10m80.iq
11 10m80 40|1552954365|11-10m80.iq
4 20m 40|2594131925|4-20m.iq
11 20m 40|2779934507|11-20m.iq
4 26m667 16 128 1|3388792897|4-26m667.iq
11 26m667 16 128 1|1271511239|11-26m667.iq
4 20mq4 16 128 1|3789273049|4-20mq4.iq
11 20mq4 16 128 1|2081653259|11-20mq4.iq
DUMPS

# command | fill ceiling | encoder mean ceiling | live ceiling | slack floor
# Cycles, or cycles/frame for encoder columns. All gates precede the board run.
while IFS='|' read -r args fill mean live slack; do
    printf '\nPASS first: dvbpace %s; zero late/underrun; fill <=%s, encoder mean/live <=%s/%s, slack >=%s cycles.\n' \
        "$args" "$fill" "$mean" "$live" "$slack"
    board "dvbpace $args"
    rc=$?
    check pace "dvbpace $args" "$fill" "$mean" "$live" "$slack"
    report "dvbpace $args" "$rc" "$?"
done <<'PACES'
11 26m667 30|40137|254607|254607|7083
4 26m667 30|40137|254607|254607|7083
11 20m 30|46769|188283|192748|150
13 20ms6 30|41318|172438|174975|5600
17 5m 30|72766|905515|905515|12842
11 20mq4 30|40137|339476|339476|7083
PACES

while IFS='|' read -r args floor; do
    printf '\nPASS first: wifimem %s; ESP_OK, healthy encoder, no playback, up/min internal heap >=%s B.\n' "$args" "$floor"
    board "wifimem $args"
    rc=$?
    check wifi "wifimem $args" "$floor"
    report "wifimem $args" "$rc" "$?"
done <<'WIFI'
11 26m667|30000
17 5m|20000
8 26m667|30000
8 20mq4|20000
WIFI

printf '\nEvery result:\n'
cat "$OUT/summary.tsv"
printf '\n%s passed, %s failed. Records and 22 saved dumps and decodes: %s\n' "$passed" "$failed" "$OUT"
(( failed == 0 ))
