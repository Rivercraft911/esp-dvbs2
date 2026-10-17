#!/bin/bash
# Coordinator only, already flashed. About five minutes; keep going after failures.
set -uo pipefail
cd "$(dirname "$0")/.."
PY=${PY:-python3}
RX=${RX:-/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb}
OUT=${1:-notes/pv-spi-board-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$(dirname "$OUT")" || exit 1
mkdir "$OUT" || exit 1
exec > >(tee "$OUT/console.log") 2>&1
board() { "$PY" host/board.py "$1" --port "${PORT:-/dev/cu.usbmodem101}" --timeout 60 --out "$OUT/runs.jsonl" "${@:2}"; }
failed=0
step() {
    local label=$1; shift
    if "$@"; then printf 'PASS %s\n' "$label" >> "$OUT/summary.txt"
    else printf 'FAIL %s\n' "$label" >> "$OUT/summary.txt"; failed=1; fi
}
trap 'board "spiinput off" || true' EXIT
printf 'PASS requires matching pattern/CRC chain, zero unexpected SPI errors, decoded TS, unchanged regression CRCs, no late banks. At 9 Mb/s, dummies are expected.\n'
printf 'Radio off. One bare board. Disconnect CM5 and all SPI jumpers before loopback.\n'
step info board info
step pins board "spipins ${PINS:-devkit}"
step pv-receive board 'spirx 3000 pv loop 9'
for fault in 1 2 3; do step "pv-fault-$fault" board "spirx 1000 pv loop 9 $fault"; done
if board 'spiinput pv loop 12'; then
    step pv-dump board 'dvbdump 13 5m 40 128 1' --save "$OUT/pv.iq"
    step pv-decode "$PY" host/spi_input.py decode "$OUT/pv.iq" --kind ts --pv --modcod 13 --sps 8 --rate 40000000 --frames 40 --receiver "$RX"
else printf 'FAIL pv-setup\n' >> "$OUT/summary.txt"; failed=1; fi
if board 'spiinput pv loop 9'; then step pv-pace board 'dvbpace 13 10m 30'
else printf 'FAIL pv-pace-setup\n' >> "$OUT/summary.txt"; failed=1; fi
step quad-receive board 'spirx 1000 gcs loop 46.6'
step input-off board 'spiinput off'
step regression-qpsk board 'dvbdump 11 26m667 16 128 1' --save "$OUT/reg-qpsk.iq"
step regression-8psk board 'dvbdump 13 20m 40 128 1' --save "$OUT/reg-8psk.iq"
step gates "$PY" host/spi_input.py check "$OUT/runs.jsonl" --folder "$OUT" --firmware "$(git describe --always --tags --dirty)"
printf '\nSummary (%s)\n' "$OUT"
cat "$OUT/summary.txt"
exit "$failed"
