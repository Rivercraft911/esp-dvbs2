#!/bin/bash
# Coordinator only. Run after flashing. No flash, reset, NVS write or radio keying here.
set -euo pipefail
cd "$(dirname "$0")/.."
PY=${PY:-/Library/Frameworks/Python.framework/Versions/3.13/bin/python3}
RX=${RX:-/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb}
OUT=${1:-notes/spi-in-board-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$(dirname "$OUT")"
mkdir "$OUT"
exec > >(tee "$OUT/console.log") 2>&1
board() { "$PY" host/board.py "$1" --port "${PORT:-/dev/cu.usbmodem101}" --timeout 90 --out "$OUT/runs.jsonl" "${@:2}"; }
trap 'board "spiinput off" || true' EXIT
printf 'Radio-off gates in docs/spi-input.md. Bare board, nothing on the selected SPI pins.\n'
board info
board "spipins ${PINS:-devkit}"
for rate in 46.6 87; do board "spirx 3000 gcs loop $rate"; done
for fault in 1 2 3 4 5; do board "spirx 500 ts loop 87 $fault"; done
for kind in gcs ts; do
    board "spiinput $kind loop 55"
    board 'dvbdump 11 26m667 16 128 1' --save "$OUT/$kind.iq"
    "$PY" host/spi_input.py decode "$OUT/$kind.iq" --kind "$kind" --receiver "$RX" > "$OUT/$kind.decode.json"
    board 'dvbpace 11 26m667 30'
    board 'wifimem 11 26m667'
done
board 'spiinput gcs loop 55 6'
board 'dvbpace 11 26m667 2'
board 'spiinput off'
board 'dvbdump 11 26m667 16 128 1' --save "$OUT/reg-qpsk.iq"
board 'dvbdump 13 20ms6 40 128 1' --save "$OUT/reg-8psk.iq"
board 'dvbpace 11 26m667 30'
"$PY" host/spi_input.py check "$OUT/runs.jsonl"
"$PY" - "$OUT" "$(git describe --always --dirty)" <<'PY'
import json, pathlib, sys, zlib
folder = pathlib.Path(sys.argv[1])
rows = [json.loads(s) for s in (folder/'runs.jsonl').read_text().splitlines()]
assert any(r.get('firmware') == sys.argv[2] for r in rows), 'wrong firmware'
for command, file, expected in (
    ('dvbdump 11 26m667 16 128 1', 'gcs.iq', None),
    ('dvbdump 11 26m667 16 128 1', 'ts.iq', None),
    ('dvbdump 11 26m667 16 128 1', 'reg-qpsk.iq', 1271511239),
    ('dvbdump 13 20ms6 40 128 1', 'reg-8psk.iq', 3178964763)):
    data = (folder/file).read_bytes()
    crc = zlib.crc32(data)
    matches = [r for r in rows if r.get('command') == command and r.get('crc32') == crc]
    assert matches, (file, crc)
    assert any(r.get('host_crc32') == crc and r.get('binary_bytes') == len(data) for r in rows), file
    if expected is not None: assert crc == expected, (file, crc, expected)
print('SPI gates, received/USB/file CRCs and synthetic regression CRCs: passed')
PY
