# Fractional fill experiment

This peripheral-free app tests three kernels: 8PSK at 3 samples/symbol and
QPSK/8PSK at 12/5 samples/symbol. It checks 243 block cases and 11,664 guarded
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
dumps with Python, then independently decodes six 16-frame streams. The CRCs
in `tests/data/moon_waveform_crcs.json` use N0CALL, peak 128, and padding to a
whole input block. They describe host dumps, not board measurements.

These candidates fail the timing/RAM admission gates. They are not live
transmitter modes. `membench moon` is an isolated coordinator probe; the normal
transmitter and its waveforms retain their existing selection. Long findings,
raw logs and the RAM account are in ignored `notes/moon-fill-report.md`.
