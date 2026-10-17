# Two-core moonshot, offline

None of the three roll-off 0.20 modes meets the current 40,137-cycle/core/bank model. Correct host streams are established; silicon timing and Wi-Fi heap are unknown. Source `53406cc`, encoder `910da2d`, fill `d655793`; origin/main `da97362` was fetched and merged. No serial port, board, flash or radio was accessed.

| Mode, 9/10 | Core 0, cycles/bank | Core 1, cycles/bank | Change needed with this split |
|---|---:|---:|---|
| 8PSK 26m667 | 56,242 | 78,155 | Kernel 51 to <=8.05 cycles/block, 84.2% fewer |
| QPSK 33m333 | 51,070 | 59,233 | Kernel 48 to <=22.98 cycles/block, 52.1% fewer |
| 8PSK 33m333 | 57,860 | 100,869 | Packing/handoff/split must improve too; zero kernel still exceeds core 0 by 1,741 cycles |

These are uncalibrated instruction/capacity estimates, not measured board deadlines. They include real input packing, physical wraps, two job dispatches where needed and duplicated chunk history. Encoder costs retain the encoder front's estimated 10k-cycle input copy. Cache/SRAM contention and Wi-Fi interrupt costs have no established bound. The supplied 48.6k 3-sps fill estimate used roll-off 0.35; the required 0.20 pulse needs a fourth table part. The current 8PSK table/ring allocations also exceed the previous measured Wi-Fi headroom. QPSK fits its shared store, but needs timing work and a real heap probe.

The handoff probe executes 32 FreeRTOS notifications: 340..909 instructions/dispatch, maximum 1,331 single-issue cycles including redirects, excluding interlocks. The capacity model uses a 2,396-cycle/job allowance on each core. QEMU CCOUNT is unused. Raw traces stay in ignored notes/; hashes are recorded in runs.jsonl. Repeated QEMU executions can take different scheduler paths.

All 57 retained CRCs plus the encoder fixture pass. Six new 16-frame streams match Python sample for sample; LeanDVB recovers 14 QPSK or 13 8PSK frames without corrections/errors. QEMU passes 324 block cases and 15,552 guarded sample slices. Firmware builds. The optional ESP-IDF component manager's process scan is sandbox-blocked here; this all-local-dependency build used `IDF_COMPONENT_MANAGER=0`.

After activating ESP-IDF v6.1, reproduce the main model:

```sh
IDF_COMPONENT_MANAGER=0 idf.py -C firmware build
xtensa-esp32s3-elf-objdump -d --no-show-raw-insn \
  firmware/build/esp-idf/main/libmain.a > notes/moon-sys/two-core.dis
python3 host/moon_schedule.py notes/moon-sys/two-core.dis
```

For the optional dispatch trace, build the existing peripheral-free test project with `-D MOON_HANDOFF=1 -B "$PWD/notes/moon-sys/handoff-build"`. In that build directory, use `python -m esptool --chip esp32s3 merge-bin -o ../handoff-flash.bin --fill-flash-size 4MB @flash_args`. Run QEMU with `-machine esp32s3 -nographic -monitor none -drive file=notes/moon-sys/handoff-flash.bin,if=mtd,format=raw -serial file:notes/moon-sys/handoff-qemu.log -d in_asm,exec,nochain -D notes/moon-sys/handoff.trace`; stop at QEMU-END. Disassemble its ELF to notes/moon-sys/handoff.dis. Add `--handoff-trace notes/moon-sys/handoff.trace --handoff-disassembly notes/moon-sys/handoff.dis` to the model command. Rebuild without MOON_HANDOFF for the kernel correctness test.

[Schedule, derivations, RAM and coordinator criteria](../../docs/moonshot.md). The coordinator runs `host/check.sh --moonshot --plan`, then `PYTHON=/path/to/numpy-python RX=/path/to/leandvb host/check.sh --moonshot notes/moon-board` after building/flashing the committed revision with N0CALL stored. It continues after failures, never flashes or transmits, and expects the current targets to fail timing/RAM. Target paces keep Wi-Fi NULL/promiscuous receive up. Silicon and a later authorized cabled RF test remain the judges.
