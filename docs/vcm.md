# Changing MODCOD while sending

The prototype switches at PLFRAME boundaries, at a fixed symbol clock. `vcm on`
allows code-rate changes within the run's initial modulation. A pattern such as
`vcm 4 4 7 11` repeats four frames per QPSK mode. `vcm mixed` allows live
QPSK/8PSK changes at **10 Msym/s only**; a pattern containing both modulations
selects that scope automatically. `vcm off` restores the fixed-mode path.
Settings last until changed or rebooted. The first pattern entry must match the
run's initial MODCOD. Pilots, normal FECFRAME length and roll-off 0.35 stay fixed.

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
| 5 Msym/s | 8,192 + 65,536 B | Same modulation only |
| 10 Msym/s | 4,096 + 32,768 B | Same modulation or mixed MODCOD 4..17 |
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

The board rejected the original mixed 5 Msym/s allocation on `d9520e8`
(radio off, coordinator records in `notes/vcm/`). Those records did not capture
free heap or largest blocks. Mixed 5 Msym/s is now rejected before allocation.
The pulse table stays internal; moving it to PSRAM would require a board fill
measurement. Same-modulation runs use only their own constellation's table.

The revised allocator borrows the existing 65,536 B store for the table, render
window, overlap, dummy and 8,144 B fast workspace, then fits as many packed frame
slots as remain, capped at three. If fewer than two fit, it reserves two frames
in PSRAM. The maximum-size FEC context is always in PSRAM. No large contiguous
internal ring or context allocation is required.

| Run | Store used | Internal data heap | PSRAM data heap | Queue |
|---|---:|---:|---:|---|
| QPSK, 1 Msym/s | 44,688 B | 0 B | 291,216 B | 8 raw PSRAM frames |
| QPSK, 5 Msym/s | 62,144 B | **0 B** | 17,696 B | 2 internal packed frames |
| QPSK, 10 Msym/s | 60,096 B | 0 B | 17,696 B | 2 internal packed frames |
| 8PSK, 5 Msym/s | 65,536 B | 10,762 B | 49,776 B | 2 PSRAM packed frames |
| 8PSK, 10 Msym/s | 55,424 B | 0 B | 39,936 B | 2 PSRAM packed frames |
| Mixed, 10 Msym/s | 55,424 B | **0 B** | 51,040 B | 2 PSRAM packed frames |

These are S3 allocation calculations, excluding allocator overhead, task stacks,
input drivers, Wi-Fi and the two 65,536 B DAC banks. QPSK at 5 Msym/s is the first
check: it reserves no internal data heap, even with a fragmented heap. Task
creation still needs its normal stack/TCB allocation. Coordinator runs on
`be6c23c` paced QPSK 5 Msym/s and mixed 10 Msym/s for 30 seconds with no late
fills; mixed fill peaked at 44,208 cycles with 41,133 cycles of slack.

Allocation failures report `stage`, `requested_bytes`, `memory`, current
`free_internal`, `largest_internal`, `free_psram`, `largest_psram`, pre-layout
`free_internal_before`/`largest_internal_before`, and `store_bytes`, before
cleanup. Successful runs report `vcm_scope`, `vcm_table_bytes`, `vcm_store_bytes`,
actual queue memory/count and pre-layout heap values, alongside existing heap
and timing counters. The coordinator supplies the real heap readings.

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
pattern, or outside the modulation(s) reserved at startup. Bad CRC, invalid length/MODCOD and duplicate
messages follow the existing rejection rules. There is no per-control ACK or
automatic receiver feedback; the run result reports controls, rejections,
changes and encoded frame counts indexed by MODCOD 4..17. Boundary-cycle counters
include FEC reprepare; total encoder work includes that cost with external input too.
This is VCM with a manual ACM command input, not a closed feedback loop.

The laptop sender polls `--control /tmp/modcod` between PV messages. Put a decimal
MODCOD in that file; replace it atomically while sending. For example, the
following is **radio off**:

```sh
printf '4\n' > /tmp/modcod
python3 examples/usb-send/send.py send input.bin --rate 5m --seconds 60 --control /tmp/modcod
# In another shell, during that run:
printf '7\n' > /tmp/modcod.new && mv /tmp/modcod.new /tmp/modcod
```

`--mixed --rate 10m --control /tmp/modcod` enables live modulation changes.
Without `--mixed`, cross-modulation controls are rejected. `--kind gcs` sends raw
bytes; `--kind file` (the default for input.bin) adds file recovery padding and
SHA-256. `--kind ts --control ...` is rejected before opening
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
independently decodes IQ. All 14 MODCODs alternate at 10 Msym/s; same-modulation patterns run at
5/10 Msym/s, and all eight QPSK MODCODs alternate at 1 Msym/s. The 28-frame
mixed patterns recover 26 contiguous frames, 156,066 bytes, with no reported BCH corrections/failures or LDPC flips.
A 56-frame pattern recovers 54; an in-band QPSK-to-8PSK control recovers 45 frames
over the nominal 32-QPSK-frame dump interval. Tests also cover startup allocation
failures, exact fragmentation diagnostics, repeated runs, split ingress reads
and physical bank wraps. QPSK 5/10 Msym/s and mixed 10 Msym/s run with the host
internal data heap bounded to zero; the 5 Msym/s 4/7/11 first-run pattern is
covered. Existing fixed-mode golden CRC files are unchanged. These are host tests, not board or RF
evidence. A receiver's VCM support is required; fixed symbol rate alone does not
guarantee seamless reception in noise.

The `be6c23c` coordinator dumps each decode as 63 contiguous MODCOD-4 frames,
matching the Python BBHEADER/data model exactly. They do not establish VCM on
hardware: `wifimem` overflowed main's stack and rebooted, clearing the pattern
before capture. Check `vcm:true` and `mode_frames` in each new run result.

VCM pulse setup now allocates its temporary taps in PSRAM. Its compiled Xtensa
stack frame fell from 1,504 to 64 B; it previously nested a 1,440 B tap array
above `shaper_build`'s 960 B bias array on main's 3,584 B stack. The completion
record reports `main_stack_bytes` and `main_stack_min_free`, in bytes since boot.
`wifimem` also samples `main_stack_min_free_before`, `_after_encoder` and
`_after_wifi` to distinguish pulse setup from Wi-Fi startup. Actual high-water
readings for the fix come from the coordinator, not the host RTOS stubs.
On `438c933`, both `wifimem` probes succeeded with VCM enabled; main retained
1,292 B. The QPSK 5 Msym/s dump independently decodes as 63 contiguous frames
0..62 (MODCOD 4/7/11: 23/20/20), and mixed 10 Msym/s as 62 frames 0..61
(4/11/13/17: 16/16/16/14). Every recovered BBHEADER/data bit matches
`host/dvbs2.py`, CCM cleared, with zero reported LDPC flips, BCH corrections or
failures. The terminal capture edge is excluded; only recovered frames are
compared. Raw dump CRCs match the board records. These are radio-off captures.

`host/decode.py` also provides `payload_sequence` and `vcm_match` for contained
RF checks. They preserve one PRNG across variable payload sizes and require
every recovered PLS mode, BBHEADER and payload at consecutive model indices.
Acquisition may discard a prefix/suffix; internal gaps, repeated frames and
wrong pattern phase fail. A passing pattern test must observe every requested
MODCOD. The local coordinator E200 helper is `notes/e200_vcm.py`; its `--plan`
and `--decode-only` paths access no devices. Both capture paths require `--key`.

Only the coordinator runs board steps. After flashing, keep the radio off:

```sh
python3 host/board.py "spiinput off"
python3 host/board.py "vcm 4 4 7 11"
python3 host/board.py "wifimem 4 5m"
python3 host/board.py "dvbdump 4 5m 64" --save /tmp/vcm-qpsk.iq --timeout 120
python3 host/board.py "dvbpace 4 5m 30" --timeout 120
# Then the mixed layout, still radio off:
python3 host/board.py "vcm 4 4 11 13 17"
python3 host/board.py "wifimem 4 10m"
python3 host/board.py "dvbdump 4 10m 64" --save /tmp/vcm-mixed.iq --timeout 120
python3 host/board.py "dvbpace 4 10m 30" --timeout 120
```

Require zero overlap mismatches, no encoder failure/underruns, positive fill
slack, zero late/fatal counters and at least 256 B encoder stack reserve. Any
keyed check is separate, only in the closed Faraday box, with the station's real
callsign, after those checks pass.
