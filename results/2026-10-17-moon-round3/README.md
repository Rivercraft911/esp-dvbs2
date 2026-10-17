# Compact split, round three

QPSK 9/10 at 100/3 Msym/s passes the combined instruction model at 38,903 /
38,948 cycles per bank on cores 0 / 1. This is not silicon admission. No serial
port, flashing, Wi-Fi or radio operation occurred. The interrupted e0766d4
board run is invalid and is not a timing input.

Source revision: 78354abbe5ab0482660947f7e57f2afb47d4d982. Offline firmware build: 3edc683.
Both cfd012c and the actual seven-symbol follow-up 73136ce are merged.
[runs.jsonl](runs.jsonl) identifies modeled and host/QEMU validation records
separately and pins source/trace hashes. Raw logs remain in ignored
notes/moon-sys/round3-*. The [integrated account](../../docs/moonshot.md)
contains the equations, allowances, memory requests and coordinator command.

The direct-quad QPSK loop costs 116 modeled cycles per 48 samples and needs
27,648 B of tables. A 2/7 helper split models 13,379 helper-body cycles per
worst bank. All 819 physical bank offsets are enumerated. The long-loop model
now includes its final two instructions on every iteration. Dispatch, edges,
SRAM traffic, interrupts and MULL timing remain predictions for the board.

All 57 retained CRCs plus the encoder fixture pass. Six complete new streams
match independent Python convolution at every sample and across physical-bank
wraps. Their new golden CRCs reflect the seven-symbol fractional pulses and
compact factoring; the 3-sps 8PSK path uses its actual six-symbol .35 pulse and
BBHEADER. All six receiver checks pass with zero BCH/LDPC corrections and
payload errors. Receiver input appends 4,096 zero samples after the pinned dump.
QEMU passes 324 block cases, 15,552 slices and 20,736 direct-quad slices.
The firmware and peripheral-free test app build with ESP-IDF v6.1 and
IDF_COMPONENT_MANAGER=0; default component-manager process enumeration is
blocked by the sandbox. No dependencies or SDK configuration changed.

8PSK still fails: 3 sps models 50,469 / 53,660 cycles; 2.4 sps models
60,441 / 62,897. Its extra table/ring/window heap requests are 53,680 /
56,752 B. Relative to the previous 47,291 B Wi-Fi headroom, at least
26,389 / 29,461 B must be reclaimed to retain 20,000 B, before helper/linked
RAM changes. At 2.4 sps, even removing all kernel work leaves core 0 at
45,583 modeled cycles with this split. Packing/staging/dispatch and memory
must improve too. No slower restoration kernel was added just to shrink RAM.

Reproduce host gates with tests/test_stream.py, tests/test_rational.py,
tests/test_moon_fill.py, tests/test_board_lock.py, tests/test_check.py and
tests/test_shaper_cycles.py. For the model, disassemble
firmware/build/esp-idf/main/libmain.a with xtensa-esp32s3-elf-objdump -d
--no-show-raw-insn, then run host/moon_schedule.py on that file.
The QEMU procedure is in tests/moon_fill/README.md.

After building/flashing the committed revision with N0CALL stored:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round3-board
```

The runner refuses existing native/reset-bridge serial owners before reset or
open, reserves the session across commands, saves host and board revisions,
and continues after command failures with a final summary. It never flashes
or transmits. The two 8PSK gates are expected to fail. Only a valid QPSK
Wi-Fi-up pace, complete-bank maximum <=40,137 cycles, zero lateness/underruns,
and free/minimum heap >=20,000 B can admit the first mode.
