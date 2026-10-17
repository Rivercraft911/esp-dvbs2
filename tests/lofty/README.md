# 8PSK offset queue

`python3 tests/test_lofty.py --receiver /path/to/leandvb` checks the actual
transmitter and all new DAC samples against Python, allocation failures,
repeated commands, sequence wraps and three independent 16-frame decodes.
It also checks the native kernel hashes against independent convolution.
All retained waveforms remain in `tests/test_stream.py`.

With ESP-IDF v6.1 active:

```sh
IDF_COMPONENT_MANAGER=0 idf.py -C tests/lofty build
(cd tests/lofty/build && python -m esptool --chip esp32s3 merge-bin \
  -o flash.bin --fill-flash-size 4MB @flash_args)
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/lofty/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/lofty-qemu.log -monitor none
```

Stop at `QEMU-END`. Pass the log with `tests/test_lofty.py --qemu LOG`.
Require 4,320 guarded sample slices, 36 mapper frames (all code rates and
both pilot settings), zero errors and hashes equal to Python. Prepared
offsets are also checked at every destination word alignment.

Rebuild with `-DLOFTY_TRACE=ON` for short encoder/render traces. Add QEMU
`-d in_asm,exec,nochain -D notes/lofty.trace`; disassemble the same linked
ELF with `xtensa-esp32s3-elf-objdump -d --no-show-raw-insn` and run:

```sh
python3 host/lofty_cycles.py notes/lofty.trace notes/lofty.dis
```

The trace sentinels mask interrupts only inside each kernel interval. The
live transmitter does not. Replay counts executed instructions, per-lane
PIE hazards and taken redirects. It never uses QEMU CCOUNT as timing.
Cache/bus contention, real input, Wi-Fi, heap fragmentation and keyed
qualification require the [coordinator](../../docs/lofty.md).
