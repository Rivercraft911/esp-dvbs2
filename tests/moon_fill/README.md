# Fractional fill experiment

This peripheral-free app tests four kernels: 8PSK at 3 samples/symbol with
roll-off .35/.20, and QPSK/8PSK at 12/5 samples/symbol with .20. It checks
324 block cases and 15,552 guarded
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

The transmitter now selects the .20 kernels through the two-core path. They
remain experimental and fail the timing/RAM admission model. `membench moon`
compares the original three isolated kernels; `tests/test_rational.py` checks
the integrated encoder/ring/fill and six .20 LeanDVB streams. Admission belongs
to `host/check.sh --moonshot`. The dispatch trace probe is `moon_handoff.c`;
[build and replay instructions](../../results/2026-10-17-moon-two-core/README.md)
separate executed instruction counts from silicon timing. Raw logs stay in notes/.
