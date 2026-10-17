# Radio and bank geometry

The transmitter uses the S3 radio's undocumented DAC test engine. It reads
internal SRAM while core 1 fills the inactive bank. The implemented register
sequence is independently written; findings and pinned source links are in
[prior-art.md](prior-art.md) and `radio_s3.c`.

## Radio bring-up

Wi-Fi owns PHY initialization: NULL mode, start, power save off, promiscuous
receive and channel 1. The worker stops the DAC, reclaims bank ownership,
unkeys, selects the direct local oscillator and sets the wide channel mode.
It fills both banks before starting samples.

The retained chain is the measured `cwgain` sequence: enable the vendor CW
chain, stop its tone, move only the PLL, restore direct LO, program gain slot
zero and cancel the front end's base index. Read back the selected slot and
abort if it is not zero. Clear tone override, grant bank ownership, start the
DAC, then set its held trigger. The epoch is read between the final two writes.
Cleanup stops the DAC, stops vendor CW, unkeys, removes overrides, reclaims
banks and stops/deinitializes Wi-Fi on every exit.

The old directly forced chain enabled only I on this board. CW alone varied
in level; pinning gain made it repeat within 0.5 dB. At +1 MHz the measured
image was -35 dBc and carrier leakage about -45 dBc. Gain is a vendor index,
0..17, not calibrated dBm. [RF measurements](../results/2026-10-16-radio/README.md).
The unused `forced`, `cw` and `cw40` alternatives and console selector are gone.
Tone, two-tone, tagged PRBS, I/Q-only, DC and gap diagnostics remain because
`host/bench.py`, `capture.py` and their tests use them.

## Geometry and deadline

Two physical 64 KiB banks at 0x3fcc0000 and 0x3fcd0000 are reserved before the
heap starts. A bank plays 16,380 complex samples, 4 B each; the final four
physical words are unused. Never grant physical bank 3, which contains ROM data.
The linker checks both the DRAM range and its instruction-address alias.

The engine retains its offset across bank switches. Sample n occupies physical
word `n % 16380` in either bank. For handoff `h = k * STEP`, bank k contains
`[h - LEAD, h - LEAD + BANK)` and plays `[h, h + STEP)`:

```
LEAD + STEP + TAIL = BANK = 16380 samples
period = STEP / Fs seconds
deadline = STEP * (240e6 cycles/s) / Fs cycles
```

| DAC Fs | LEAD | TAIL | STEP | Period | Deadline at 240 MHz |
|---|---:|---:|---:|---:|---:|
| 40 MS/s | 64 | 2,048 | 14,268 | 356.7 us | 85,608 cycles |
| 80 MS/s | 128 | 512 | 15,740 | 196.75 us | 47,220 cycles |

The 40 MS/s overlap was inherited from prior measurements. At 80 MS/s the lead
doubles to retain its time allowance, while the smaller tail gives 10.3% more
cycles per bank than the former 42,804-cycle schedule. Both geometries are
covered by the actual dry-run worker in the radio QEMU harness. This proves
the software timeline, not the DAC's physical read position.

A switch aims LEAD/2 samples early. Interrupt levels 1..3 are masked from the
end of filling through selection, moving the 1 kHz tick into fills. Earlier
runs had about 0.31% late switches, consistent with that tick; the observed
interrupt mechanism was inferred from timing, not measured directly. Levels
4/5 and NMI remain enabled (ESP32-S3 TRM v1.8, 9.3.2, Table 9.3-2, pp. 544–545).

Let d be the DAC's read-position lag relative to the CPU model, samples, and
lambda the switch delay past its aim, CPU cycles. With C cycles/sample, the
model is safe when `d <= LEAD/2` and `lambda/C - LEAD/2 - d < TAIL`.
The code stops at `(TAIL - LEAD) * C` cycles after the nominal handoff,
reserving one lead for unknown read-ahead. This allowance is not extra steady
compute capacity. Empty fills cost zero in this model; fills at the full
deadline leave zero slack. A continuous fill over the deadline cannot be
rescued by overlap indefinitely.

The CPU-to-DAC start latency, bank-selection latency and sample continuity
still need an external capture. `tx ... gap` silences each bank's lead and tail;
the model predicts `abs(actual_switch_sample - h)` silent samples. Tests check
every switch position inside both overlaps; a cabled capture must test the
hardware prediction. This command transmits and requires operator authorization.

## Evidence and acceptance

Main keyed QPSK 9/10 at 80/3 Msym/s and 8PSK 2/3 and 5/6 at 20 Msym/s for
30 s each in a shielded box, with every bank deadline met. That establishes
playback timing, not receiver decoding or RF quality.
[QPSK](../results/2026-10-17-keyed-qpsk/README.md),
[8PSK](../results/2026-10-16-keyed-20m/README.md).
The shortened 8PSK pulse selected by cleanup has radio-off evidence only.

After the coordinator builds and flashes, `host/check.sh` prints every gate
before running and continues after failures. It never flashes, retunes, keys,
changes NVS or invokes an SDR. It uses stored N0CALL and verifies the firmware
revision. `--plan` opens no devices.

The slim check verifies all 58 golden CRCs on the host, nine board dumps
(six 16-frame fast-mode dumps and three regressions), each decoded by LeanDVB,
140 FEC CRCs, four key 30 s paces, four 10 s regression paces and three Wi-Fi
memory probes. `--all-crcs`
additionally dumps/decodes every golden configuration on the board and is much
slower. The script writes per-command logs, saved IQ/decoder JSON, `runs.jsonl`
and `summary.tsv` to a fresh notes directory. LeanDVB must recover at least
N-2 QPSK frames. For 8PSK it permits at most one leading acquisition frame
and three trailing EOF frames, requiring at least N-4 contiguous frames, all
bit-exact with zero error/correction counters. The local receiver returns
36–37 of 40 known-correct 8PSK host frames. This is a receiver pipeline/acquisition
allowance, not payload error tolerance.
Exit status is nonzero if any
check fails. Preserve accepted measurements in a new results directory;
never rewrite old evidence.

Every retained modulation/rate path has a paced representative. The four
critical paths (QPSK 20m/26m667, 8PSK 5m/20m) run for 30 s; the remaining
regressions run for 10 s against the same 30 s baseline ceilings. Fill maximum,
encoder mean and encoder live maximum must each be at most
`floor(baseline * 1.02)`, with zero underruns/late fills/late switches/fatal
lateness and positive slack. Baselines are read from immutable next4, next5
and qpsk-goal records, using the adopted kernel's old name. The older
20msym records document the replaced nine-symbol 20m path, not its new ceiling.
The full encoder runs; startup is excluded only from the separately reported
live maximum. No timing threshold is inferred from host clocks.

`wifimem` allocates the exact transmitter layout, prefills the encoder, starts
and stops Wi-Fi, then returns before radio-register access. No samples play.
The tight 8PSK 9/10 at 5 Msym/s floor is **22,351 B**, both Wi-Fi-up free heap
and low-water heap, from [main's probe](../results/2026-10-17-qpsk-goal/runs.jsonl).
The other floors retain measured QPSK block and short-8PSK heap values.
Passing a linker-size check does not establish Wi-Fi startup or free heap.
