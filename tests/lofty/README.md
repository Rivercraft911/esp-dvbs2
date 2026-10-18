# 8PSK queue checks

`python3 tests/test_lofty.py --receiver /path/to/leandvb` checks all new DAC
words against the independent Python encoder and convolution, allocation
failure cleanup, repeated commands, counter wraps and three 16-frame decodes.
The kernel tests cover 4,320 guarded slices, 36 resident-cache mapper frames,
96 QPSK sign-mapper frames and 4,095 fused QPSK render blocks. Context guards
use each mode's actual allocation size. Native clocks and heap are synthetic.
Warm-up must preserve the first payload and all frame seams. The PIE run
checks resident BCH against canonical parity and installs/restores real
software interrupt handlers on both CPUs twice, including same-bank notify
accounting. Its synthetic observer lengths are not timing measurements.

`python3 tests/test_lofty_handoff.py /tmp/lofty-handoff` replays the supplied
ID-to-data bank through the real station mux and overlap cache. It checks the
8,100/6,092-sample callbacks, bulk-render eligibility and two exact data frames.

With ESP-IDF v6.1 active:

```sh
IDF_COMPONENT_MANAGER=0 idf.py -C tests/lofty -DLOFTY_TRACE=OFF build
(cd tests/lofty/build && python -m esptool --chip esp32s3 merge-bin \
  -o flash.bin --fill-flash-size 4MB @flash_args)
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/lofty/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/lofty-qemu.log -monitor none
```

Stop at `QEMU-END`. `tests/test_lofty.py --qemu notes/lofty-qemu.log` checks
all table/sample hashes against Python, then runs the native checks. Require
zero errors, intact guards and the 96-frame/4,095-block success messages.

Rebuild with `-DLOFTY_TRACE=ON` for two successive frames at each of MODCODs
13, 15, 17 and 11. This covers both 8PSK halfword offsets and persistent page
positions. Add QEMU `-d in_asm,exec,nochain -D notes/lofty.trace`; disassemble
the linked ELF with `xtensa-esp32s3-elf-objdump -d --no-show-raw-insn`, then:

```sh
python3 host/lofty_cycles.py notes/lofty.trace notes/lofty.dis
```

The 49 intervals include six stages per frame (first publication separate
from remaining publication) and one renderer. Sentinels mask interrupts only
inside this fixture. Live firmware keeps interrupts enabled. Replay counts
instructions, lane dependencies and redirects, never QEMU CCOUNT. Cache/bus
stalls, Wi-Fi heap and deadlines need the [coordinator](../../docs/lofty.md).
