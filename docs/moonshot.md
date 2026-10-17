# Two-core experiment

**QPSK 9/10 at 33.333333 Msym/s is not admitted.** The round-five candidate
has host correctness checks and an ESP-IDF build. Silicon timing and heap
remain unmeasured. No USB access or RF operation was attempted.

Round four (`d05b368`, coordinator records in `notes/moon-round4-board/`)
failed even the retained modes: 8PSK 5/6 at 20 Msym/s averaged 312,051
cycles/frame with 4,401 underruns. QPSK at 33.333333 Msym/s averaged 613,713
cycles/frame and reached 435,957 cycles/full bank. Its measured helper bodies
account for 167,545 cycles/frame; subtracting them still leaves 446,168.
Legacy runs without a helper also inflated BB and map time. This is consistent
with cache interference from the PSRAM source, but does not prove its cause.

## Round five

Merged `origin/main` at `bf28f7b`, retaining quad SPI input and the unpack-parity
fix from `88800e8`. Legacy paces now use main's original contiguous xorshift
stream. Dumps retain their original payload and CRCs.

New-mode paces copy one precomputed internal-RAM frame in chunks of at most
1,316 B, the real ingress payload size. QPSK's 7,264 B source borrows the unused
shaper-store tail, adding no internal heap allocation. It repeats every frame.
8PSK falls back to an internal allocation when the tail is too small. Startup
generation is excluded from pacing. This fixture measures a copy and station
header, not DMA, CRC validation, descriptor handling or interrupts.

Main's [SPI board log](../results/2026-10-17-spi-in/board.log), firmware
`b9eb7d7`, reports QPSK GCS replay copy time of 57,972,998 cycles for 12,021
encoded frames: `ceil(57972998 / 12021) = 4823 cycles/frame`. The check uses
that measured mean as a planning floor alongside the actual timed payload.
It is not a worst-case copy bound. The earlier estimated 10,000-cycle input
allowance is removed.

The helper runs at priority 4, below the priority-5 encoder. QPSK 9/10 lends
core 0 between 256-byte BCH slices, LDPC rows, and QPSK map runs. Vector state
is stored before blocking. The helper publishes completion before notifying
the encoder. This avoids both priority-24 preemption and indefinite helper
starvation during a whole encoder frame. Wi-Fi, interrupts, task switching
and SRAM contention still need board measurements.

## Admission

`host/check.sh --moonshot` first runs all retained main dumps, all eight main
paces, and the existing heap probes. The four critical paces run for 30 s:
`11 26m667`, `15 20m`, `13 10m`, `17 5m`. Fill maximum, encoder mean and live
maximum must each be at most 102% of the fixed main measurements. No timing
ceiling or existing heap floor is relaxed.

A failed main gate invalidates QPSK admission even when its own pace passes.
The separate `QPSK admission` result requires its dump/decode, Wi-Fi-up pace
and heap probe. Other experimental 8PSK failures remain visible and make the
whole script exit nonzero, but cannot hide a valid QPSK result.

The exact bank deadline is
`15740 samples / 80e6 samples/s * 240e6 cycles/s = 47220 cycles`.
The 85% ceiling is **40,137 cycles/core/bank**. QPSK's frame period is
`33282 symbols / (100e6/3 symbols/s) * 240e6 cycles/s = 239630.4 cycles`;
its 85% ceiling is **203,685 cycles/frame**. Frame dimensions come from
`host/dvbs2.py`. Encoder wall mean and live maximum, including copy and helper
waits, must pass independently. Kernel time plus the measured SPI copy floor
must also pass. Core-0 capacity is screened using helper bank maximum,
4,792 scheduling cycles and frame work including at least that copy floor.
The complete bank maximum includes join waits. These screens are not a
worst-case latency proof. Wi-Fi-up and minimum free heap must be at least
**20,000 B**. Existing legacy heap floors remain gates too.

Host checks retain all 57 waveform CRCs plus the encoder fixture and six
fractional streams. QEMU checks 140 phase CRCs, 14 PIE comparisons, 896 packed
frames and 512 quad frames. The quad tests clobber all vector registers and
SAR/SAR_BYTE at 33,280 handoffs while preserving exact output. Host tests also
cover repeated source frames, allocation failures, cleanup and notification
handoffs. QEMU establishes instruction semantics, not timing.

After building/flashing the committed revision, with N0CALL stored, the board
power-cycled and all other serial owners closed:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round5-board
```

`--plan` opens no device. The runner reserves the serial session, continues
after failures and saves every result. It never flashes, writes NVS or transmits.
