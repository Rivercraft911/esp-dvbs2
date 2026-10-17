# Shaper PIE checks in QEMU

This peripheral-free ESP-IDF app checks the adopted n4/n8/n40 sample glue,
QPSK block kernels, six-symbol 8PSK pulse, ring pack/unpack and word copies.
The independent host oracles also check tables and guarded output slices.
QEMU establishes instruction semantics, never silicon timing.

After activating ESP-IDF v6.1:

```sh
idf.py -C tests/shaper_pie build
(cd tests/shaper_pie/build && python -m esptool --chip esp32s3 merge-bin \
  -o flash.bin --fill-flash-size 4MB @flash_args)
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/shaper_pie/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/shaper-pie.log -monitor none
```

Stop after `QEMU-END`. On macOS QEMU may need its SDL2 directory in
`DYLD_LIBRARY_PATH`. If sandbox process discovery fails, build with
`IDF_COMPONENT_MANAGER=0`; no managed components are required.

```sh
python3 tests/test_shaper.py notes/shaper-pie.log
python3 tests/test_tx_samples.py notes/shaper-pie.log
python3 tests/test_shaper_short.py notes/shaper-pie.log
python3 tests/test_qpsk_fill.py notes/shaper-pie.log
```

All records, hashes and guards must match the host. The quad ring also checks
65,536 packed-copy slices. Removed row-staging, five-sample and 8PSK n3
prototypes remain in git history. [Current fill design](../../docs/bank-fill.md).
