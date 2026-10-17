# Fractional fill experiment

This peripheral-free app tests four kernels: 8PSK at 3 samples/symbol and
QPSK/8PSK at 12/5 samples/symbol, with wide and compact QPSK tables.
It checks 324 block cases and 15,552 guarded
sample slices, including every fractional phase, destination alignment and a
whole bank. QEMU checks instruction semantics, never silicon timing.

After activating ESP-IDF v6.1:

```sh
IDF_COMPONENT_MANAGER=0 idf.py -C tests/moon_fill build
(cd tests/moon_fill/build && python -m esptool --chip esp32s3 merge-bin \
  -o flash.bin --fill-flash-size 4MB @flash_args)
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/moon_fill/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/moon-qemu.log -monitor none
```

Stop after `QEMU-END`. Use a host Python with numpy/scipy after leaving the IDF
environment:

```sh
python3 tests/test_moon_fill.py notes/moon-qemu.log
host/check_fill.sh --plan
```

`host/moon_fill.py --receiver /path/to/leandvb` compares complete native C
dumps with Python, then independently decodes eight 16-frame streams. The CRCs
in `tests/data/moon_waveform_crcs.json` use N0CALL, peak 128, and padding to a
whole input block. They describe host dumps, not board measurements. The receiver input appends
4,096 zero samples outside these CRCs to drain finite-input lookahead; every
recovered frame must still match the sixteen real input frames exactly.

These candidates are not live transmitter modes. Seven-symbol .20 pulses pass
the exact quantized spectrum model. Wide QPSK meets the timing model but uses
98,368 B of tables; compact QPSK uses 27,648 B and needs core 0 help. Both 8PSK
kernels also need help under the complete-fill model. No new silicon timing,
split-stream or Wi-Fi heap admission is claimed. `membench moon` is an isolated coordinator probe; the normal
transmitter and its waveforms retain their existing selection. Long findings,
raw logs and the RAM account are in ignored `notes/fill-round2.md`.

`host/moon_fill.py --design` reproduces the seven-symbol integer coefficients.
`--fallback` reports the .25/.35 mask, RAM and optimistic unassembled schedule
curve. `--disassembly FILE --baseline notes/fill-board/runs.jsonl` applies the
d655793 measured per-call residual to the new instruction model. New MULL
throughput and full-bank timing still need the coordinator's `--probe`.
