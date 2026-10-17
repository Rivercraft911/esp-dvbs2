#!/bin/bash
# Coordinator only, after flashing. No reset, flash, NVS write or radio keying.
set -uo pipefail
cd "$(dirname "$0")/.."
PY=${PY:-python3}
RX=${RX:-/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb}
OUT=${1:-notes/spi-in-board-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$(dirname "$OUT")" || exit 1
mkdir "$OUT" || exit 1
exec > >(tee "$OUT/console.log") 2>&1
board() { "$PY" host/board.py "$1" --port "${PORT:-/dev/cu.usbmodem101}" --timeout 90 --out "$OUT/runs.jsonl" "${@:2}"; }
failed=0
step() {
    local label=$1
    shift
    if "$@"; then printf 'PASS %s\n' "$label" >> "$OUT/summary.txt"
    else printf 'FAIL %s\n' "$label" >> "$OUT/summary.txt"; failed=1; fi
}
trap 'board "spiinput off" || true' EXIT
printf 'Radio off. Bare board, nothing on selected SPI pins. Demo first; replay is a compute gate only.\n'
step info board info
step pins board "spipins ${PINS:-devkit}"
# A fixed 22 Mb/s offered ceiling leaves room for the 19.36 Mb/s demo stream.
if board 'spiinput ts loop 22'; then
    step demo-dump board 'dvbdump 13 10m 40 128 1' --save "$OUT/demo.iq"
    step demo-decode "$PY" host/spi_input.py decode "$OUT/demo.iq" --kind ts --modcod 13 --sps 4 --rate 40000000 --frames 40 --receiver "$RX"
    step demo-pace board "dvbpace 13 10m ${SECONDS_PER_RUN:-10}"
else printf 'FAIL demo-setup\n' >> "$OUT/summary.txt"; failed=1; fi
# No sender task or SPI3 allocation. Actual CRC/copy/encoder; no DMA contention proof.
wifi_modes=('13 10m' '4 26m667' '11 26m667' '11 20m' '13 20m' '17 5m' '17 10m')
if [[ ${WIFI_ALL:-0} == 1 ]]; then
    wifi_modes=()
    for mc in {4..17}; do
        for rate in 5m 10m 20m; do wifi_modes+=("$mc $rate"); done
        if (( mc < 12 )); then wifi_modes+=("$mc" "$mc 26m667"); fi
    done
fi
for kind in ts gcs; do
    if board "spiinput $kind replay"; then
        step "$kind-demo-compute-pace" board "dvbpace 13 10m ${SECONDS_PER_RUN:-10}"
        step "$kind-compute-dump" board 'dvbdump 11 26m667 16 128 1' --save "$OUT/$kind.iq"
        step "$kind-compute-decode" "$PY" host/spi_input.py decode "$OUT/$kind.iq" --kind "$kind" --replay --receiver "$RX"
        step "$kind-compute-pace" board "dvbpace 11 26m667 ${SECONDS_PER_RUN:-10}"
    else printf 'FAIL %s-replay-setup\n' "$kind" >> "$OUT/summary.txt"; failed=1; fi
    if [[ $kind == gcs ]]; then
        if board 'spiinput gcs loop 22'; then
            step gcs-demo-pace board "dvbpace 13 10m ${SECONDS_PER_RUN:-10}"
        else printf 'FAIL gcs-loop-setup\n' >> "$OUT/summary.txt"; failed=1; fi
    fi
    # Receive-only allocation, no sender buffers. No arriving bytes are required.
    if board "spiinput $kind"; then
        for mode in "${wifi_modes[@]}"; do
            step "$kind-wifi-$mode" board "wifimem $mode"
        done
    else printf 'FAIL %s-receive-setup\n' "$kind" >> "$OUT/summary.txt"; failed=1; fi
done
for rate in 22 46.6; do step "throughput-$rate" board "spirx 1000 gcs loop $rate"; done
# No offered-rate gate for saturation. Report the verified rate, not an 87 Mb/s claim.
step loop-capacity board 'spirx 1000 gcs loop 0'
for fault in 1 2 3 4 5; do step "fault-$fault" board "spirx 500 ts loop 87 $fault"; done
if board 'spiinput ts loop 22 6'; then step starvation board 'dvbpace 13 10m 2'
else printf 'FAIL starvation-setup\n' >> "$OUT/summary.txt"; failed=1; fi
step input-off board 'spiinput off'
step regression-qpsk board 'dvbdump 11 26m667 16 128 1' --save "$OUT/reg-qpsk.iq"
step regression-8psk board 'dvbdump 13 20m 40 128 1' --save "$OUT/reg-8psk.iq"
step baseline-demo-pace board "dvbpace 13 10m ${SECONDS_PER_RUN:-10}"
step regression-pace board "dvbpace 11 26m667 ${SECONDS_PER_RUN:-10}"
step gates "$PY" host/spi_input.py check "$OUT/runs.jsonl" --encoder-5pct --folder "$OUT" --firmware "$(git describe --always --tags --dirty)"
printf '\nSummary (%s)\n' "$OUT"
cat "$OUT/summary.txt"
printf 'External CM5/S3 DMA and cable timing still require a separate run.\n'
exit "$failed"
