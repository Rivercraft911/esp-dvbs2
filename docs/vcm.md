# Changing MODCOD while sending

The prototype switches at PLFRAME boundaries, at a fixed symbol clock. Enable it
before a run with `vcm on`, or repeat a test pattern with `vcm 4 4 11 13 17`
(four data frames per MODCOD). `vcm off` restores the fixed-mode path. Settings
last until changed or rebooted. The first pattern entry must match the run's
initial MODCOD. Pilots, normal FECFRAME length and roll-off 0.35 stay fixed.

## Scope

A normal FECFRAME has 64,800 coded bits. With pilots, QPSK takes 33,282 symbols
and 8PSK takes 22,194. Payload capacity is
`8 * ((Kbch - 80) / 8) * Rs / frame_symbols` bit/s. Changing code rate changes
Kbch, BCH/LDPC parameters and PLHEADER, but leaves the pulse and frame duration
alone. Changing modulation also changes frame duration and interleaving. The
ring therefore publishes each slot's symbol count with its data. Neither the
symbol clock nor pulse history resets at a boundary. The sender must fit the
lowest selected capacity or accept backpressure and dummy frames.

| Symbol rate | Separate QPSK + 8PSK pulse tables | Prototype |
|---|---:|---|
| 1 Msym/s | 32,768 + 262,144 B (8PSK unimplemented) | QPSK MODCOD 4..11 |
| 5 Msym/s | 8,192 + 65,536 B | MODCOD 4..17, common 65,536 B table |
| 10 Msym/s | 4,096 + 32,768 B | MODCOD 4..17, common 32,768 B table |
| 20 Msym/s | 49,152 + 16,384 B | Rejected |
| 26.67 Msym/s | 36,864 + 36,864 B | Rejected |
| 33.33 Msym/s | 24,576 B QPSK; no corresponding 8PSK path | Rejected |

These are derived table sizes, not free-heap measurements. The legacy table is
`4 * 2^(3*b) * next_power_of_two(3 * round_up(sps, 4))` bytes. At 5/10 Msym/s,
both constellations use the same nine-symbol taps. All QPSK windows are already
resident in the full eight-phase table; allocating another QPSK table is
unnecessary. At 20/26.67 Msym/s the optimized paths use different pulses and
queue formats. Two tables alone would not preserve their boundary history.
The fractional 33.33 Msym/s path is QPSK-only, roll-off 0.20. Code-rate-only
switching is possible in principle at these higher rates, but this prototype
rejects it too: resident FEC caches and mode-sized queues need another design
and timing check.

The S3 layout reserves the existing 65,536 B store. Additional internal heap
requests are 18,116 / 44,106 / 59,744 B at 1/5/10 Msym/s, respectively. They
include maximum-size FEC context where internal, ring, window and overlap cache;
exclude allocator overhead, task stacks, input drivers, Wi-Fi and the two
65,536 B DAC banks. At 5 Msym/s the context/workspace use 30,116 B PSRAM;
at 10 Msym/s the workspace and one maximum QPSK slot share the table tail.
Allocation failure aborts before playback. These are allocation calculations;
on-chip free heap, switching cost and bank deadlines remain unmeasured.

## Stream and control

VCM accepts **GCS**, a generic continuous byte stream, including file containers.
MATYPE-1 is `0x60`: continuous GS, single input, CCM cleared, ISSY/NPD off,
roll-off 0.35. DFL follows the selected Kbch; UPL, SYNC, SYNCD and MATYPE-2 are
zero, and CRC-8 is recomputed. Payload bytes continue across frames unchanged.
TS remains supported in fixed mode. TS VCM is rejected because the existing
188-byte sync replacement and SYNCD handling do not implement the timing and
null-packet tools required by [EN 302 307-1, Table 1 note 3 and Annex D](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).

PV type **3**, on either SPI or USB, has length **1**, containing MODCOD 4..17.
All other header, padding, CRC and sequence rules are unchanged. Controls have
their own accepted/rejected counters and never enter the payload stream. A
control consumed while assembling frame N affects frame N+1; already queued
frames keep their mode. Several controls consumed before that boundary coalesce
to the latest value. Controls are rejected with VCM off, during a console
pattern, or for 8PSK at 1 Msym/s. Bad CRC, invalid length/MODCOD and duplicate
messages follow the existing rejection rules. There is no per-control ACK or
automatic receiver feedback; the run result reports controls, rejections,
changes and encoded frame counts indexed by MODCOD 4..17. Boundary-cycle counters
include FEC reprepare; total encoder work includes that cost with external input too.
This is VCM with a manual ACM
command input, not a closed feedback loop.

The laptop sender polls `--control /tmp/modcod` between PV messages. Put a decimal
MODCOD in that file; replace it atomically while sending. For example, the
following is **radio off**:

```sh
printf '4\n' > /tmp/modcod
python3 examples/usb-send/send.py send input.bin --rate 5m --seconds 60 --control /tmp/modcod
# In another shell, during that run:
printf '13\n' > /tmp/modcod.new && mv /tmp/modcod.new /tmp/modcod
```

`--kind gcs` sends raw bytes; `--kind file` (the default for input.bin) adds file
recovery padding and SHA-256. `--kind ts --control ...` is rejected before opening
USB. A dump with a console pattern counts actual data frames; a dump with live
controls uses the initial mode's nominal frame duration as its time limit.

## Receiver and checks

LeanDVB **does decode mixed MODCODs**. In `src/leansdr/dvbs2.h`,
`s2_frame_receiver` parses every PLS, chooses the constellation and predicts the
next header from that frame's slot count. `s2_fecdec` selects FEC/BCH from
each frame's PLS; the deframer accepts CCM cleared and variable DFL. Do not
restrict `--modcods` to one mode. External LDPC helpers must support every
selected mode too. Source inspected: LeanSDR `84c59e1`, with the local diagnostic
patch from the [receiver build tool](../examples/self-check/build_receiver.py).
No receiver source is linked into the transmitter.

```sh
python3 tests/test_vcm.py --receiver /path/to/leandvb
```

This runs the actual firmware lifecycle with peripheral stubs, compares every
PL symbol with `host/dvbs2.py` and every DAC word with the plain C encoder, then
independently decodes IQ. All 14 MODCODs alternate at 5/10 Msym/s; all eight QPSK
MODCODs alternate at 1 Msym/s. The 28-frame mixed patterns recover 26 contiguous
frames, 156,066 bytes, with no reported BCH corrections/failures or LDPC flips.
A 56-frame pattern recovers 54; an in-band QPSK-to-8PSK control recovers 45 frames
over the nominal 32-QPSK-frame dump interval. Tests also cover startup allocation
failures, repeated runs, split ingress reads and physical bank wraps. Existing
fixed-mode golden CRC files are unchanged. These are host tests, not board or RF
evidence. A receiver's VCM support is required; fixed symbol rate alone does not
guarantee seamless reception in noise.

Only the coordinator runs board steps. With radio off, flash, select
`spiinput off`, run `vcm 4 4 11 13 17`, then `dvbdump 4 5m 64` and
`dvbpace 4 5m 30`. Require zero overlap mismatches, no encoder failure, positive
fill slack, zero late/fatal counters and at least 256 B encoder stack reserve.
Check memory with `wifimem 4 5m` before considering keying. Any keyed check is a
separate step, only in the closed Faraday box, with the station's real callsign.
