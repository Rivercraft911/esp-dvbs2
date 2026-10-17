# Shaper PIE check in QEMU

This ESP-IDF app compares every output word of the 4-sample and 5-sample PIE kernels with C,
then emits table and sample checksums for the Python model. It does not initialize the radio.
Both roll-offs and three seeds exercise empty calls, short lengths, bank-sized calls,
the staged-row kernel, and the helpers that handle every n4 tail using PIE.
CI builds this harness; execution needs Espressif's ESP32-S3 QEMU.

From the repository root, after activating ESP-IDF v6.1:

```sh
idf.py -C tests/shaper_pie build
cd tests/shaper_pie/build
python -m esptool --chip esp32s3 merge-bin -o flash.bin --fill-flash-size 4MB @flash_args
cd ../../..
mkdir -p notes
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/shaper_pie/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/shaper-pie.log -monitor none
```

Stop QEMU after `QEMU-END` appears in the serial log. On macOS the QEMU build may need
`DYLD_LIBRARY_PATH` pointing to its SDL2 library. If the sandbox prevents the IDF component
manager's process discovery, set `IDF_COMPONENT_MANAGER=0` for the build. No managed component
is needed by this app.

```sh
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
  tests/test_shaper.py notes/shaper-pie.log
```

The checker requires all 324 cases, zero word mismatches, intact output guards, and table and
output checksums equal to `host/shaper.py`. QEMU verifies instructions, not silicon timing.

The same app now runs `tests/tx_samples.c`: 2,560 raw/scaled-phase sample-glue cases at
4/8/40 sps, every edge offset, short tail and 80 wrapping DAC-bank schedules per profile.
Pass the same log to `python3 tests/test_tx_samples.py notes/shaper-pie.log` to require
zero word mismatches, intact guards and hashes identical to the host legacy-C oracle.
The 324 table/PIE cases still compare with the independent Python shaper model.
It also checks 26,114 pack/unpack cases, including both unpack formats, and 864 word-copy
cases with guards, all pointer alignments, short lengths, overlap-sized and bank-sized copies.

For the instruction issue model and the quality/budget sweep, from the root:

```sh
xtensa-esp32s3-elf-objdump -d --no-show-raw-insn \
  tests/shaper_pie/build/esp-idf/main/libmain.a > notes/shaper.dis
python3 host/shaper_cycles.py notes/shaper.dis
python3 host/shaper_budget.py --disassembly notes/shaper.dis --qemu notes/shaper-pie.log
```

The n8 calibration is a prior radio-off measurement. The 22% stress factor comes from the
encoder's prior model error and is a sensitivity check, not a bound on shaper timing.

The four-symbol n3 experiment adds 24 cases with two history words, zero-length
calls, short groups and a full bank. It reuses the existing output buffers and
allocates one 24 KiB table while running. Check with
`python3 tests/test_maxrate.py notes/shaper-pie.log`; model its assembled loop with
`python3 host/maxrate.py --disassembly notes/shaper.dis`. The model excludes bus
contention and interrupts. See `docs/max-astra.md` for the whole-transmitter budget.
