# Shaper PIE check in QEMU

This ESP-IDF app compares every output word of the 4-sample PIE kernel with C, then emits
table and sample checksums for the Python model. It does not initialize the radio. Both
roll-offs, three seeds, 1/7/13/4093 symbols and a full 4095-symbol bank with a C tail are checked.
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

The checker requires all 30 cases, zero word mismatches, intact output guards, and table and
output checksums equal to `host/shaper.py`. QEMU verifies instructions, not silicon timing.
