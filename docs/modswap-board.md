# Mixed VCM: board decode and merge assessment

The coordinator ran **479a068**, radio off, on 2026-10-18 with
`vcm axes on`, `vcm 1 4 11 13 17`, 5 Msym/s. Source evidence is
`notes/modswap-board/runs.jsonl`, the console logs and `mixed5m.iq`.
The [committed result](measurements/2026-10-18-modswap-board.json) contains
the selected measurements, source/capture hashes and independent decode.
The agent used saved files only; it did not access the board or radio.

## Exact payload verification

The dump is **28,403,712 bytes**, signed int16 LE I/Q at **20 MS/s**, from
40 MS/s DAC words decimated by two. Its CRC-32 is **250437847**, agreeing
with both firmware and the coordinator's capture CRC. SHA-256:
`e746fa02134186da9a10ab0828ea02f3347d99af64e250d2e286b933fd1776ad`.

LeanDVB recovered **62 contiguous frames, indices 0..61**, containing
**369,990 payload bytes / 2,959,920 compared bits**, with zero payload errors,
LDPC flips/residual checks, BCH corrections/failures or rejected BBHEADERs.
PL lock remained asserted. The payload SHA-256 is
`69dcce755f369c1b3a018a1066eae088a8828b389af49c72224b54e580dee42c`.

| Actual PLS MODCOD | Frames | Exact payload bytes |
|---|---:|---:|
| 4, QPSK 1/2 | 16 | 64,256 |
| 11, QPSK 9/10 | 16 | 116,224 |
| 13, 8PSK 2/3 | 15 | 80,550 |
| 17, 8PSK 9/10 | 15 | 108,960 |

The verifier generates the continuous seed-2026 xorshift stream across the
changing 4,016/7,264/5,370/7,264-byte frame sizes, replacing each frame's first
40 bytes with the firmware's `N0CALL` station header. It builds full BBFRAMEs
with `host/dvbs2.py`, CCM cleared. **Every complete BBHEADER and payload byte**
matches a unique contiguous model interval. FEC diagnostics supply each
frame's actual PLS MODCOD; this distinguishes 11 and 17 despite equal DFL.
No per-MODCOD concatenation can conceal gaps or reordering.

The capture duration contains **64 nominal data frames**. The log's
`frames_played=65` includes bank lookahead selected while preparing later
banks, beyond the USB capture endpoint. It does not imply 65 complete frames
in the file. Two trailing frames were not returned by this receiver's EOF
pipeline; the report explicitly records them. The 4,096 appended zero samples
flush receiver lookahead and are excluded from capture CRC and length.

Reproduce entirely offline:

```sh
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 host/check_vcm_dump.py notes/modswap-board/mixed5m.iq \
  --records notes/modswap-board/runs.jsonl --receiver "$RX" \
  --output notes/modswap-board/decode
python3 tests/test_vcm_dump.py
```

Output includes `report.json`, `frames.jsonl` (frame index, MODCOD, header and
payload hash), `frames.bbf`, `payload.bin`, and `leandvb.log`. The verifier
rejects CRC/length mismatches, payload corruption, CRC-valid CCM changes,
same-size MODCOD swaps, gaps, duplicate frames, missing diagnostics and
receiver error counters. It opens no serial device and issues no board commands.

## Measured 5 M margin

| Radio-off measurement | Result |
|---|---:|
| Wi-Fi-up free internal / largest block | 77,143 / 31,744 B |
| Wi-Fi probe minimum free internal | 75,327 B |
| Playback store / shaping table | 64,192 / 10,240 B |
| Playback ring/workspace heap requests | 0 B |
| Dump overlap mismatches / max fill | 0 / 72,415 cycles |
| 30-second pace late fills / late switches / fatal | 0 / 0 / 0 |
| Pace maximum fill | 70,730 cycles |
| Minimum fill slack / bank deadline | 14,505 / 85,608 cycles (**16.94%**) |
| Encoder mean / live maximum | 499,197 / 687,389 cycles/frame |
| Encoder minimum stack reserve | 1,040 B |
| Played frames / underruns / minimum queued lead | 5,408 / 0 / 7,946 symbols |

The observed bank time agrees with the earlier 67–72k planning estimate.
Measured aggregate whole-symbol work is 32.81 cycles/symbol, compared with
32.17 issued instructions/symbol. Counts include both modulations.
The coordinator's usual 25–27 KB free-heap baseline is about 50–52 KB below
this run; it is contextual evidence, not a same-configuration allocation A/B.

## Can the axes serve higher rates?

**The five-level amplitude representation can; this per-symbol renderer cannot
meet the higher-rate deadlines by changing taps or clock alone.** A common
pulse still serves QPSK, 8PSK, headers and pilots. Each new pulse needs its own
integer tables and an exact oracle. Current code intentionally accepts only 5 M.

A straightforward four-sample variant drops the second output vector's
shift, XOR and store: about **29 + 1/6 instructions per symbol**. Applying that
optimistic count to three samples/symbol also ignores the extra output packing
needed there. These are renderer-only issue estimates, with **no** unpack,
cache-copy, interrupt, setup or SRAM-stall allowance:

| Rs / DAC Fs | CPU cycles available/symbol | Fresh symbols/bank | Optimistic renderer cycles | Bank deadline |
|---|---:|---:|---:|---:|
| 5 M / 40 M, existing kernel | 48 | 1,783.5 | 57,369 | 85,608 |
| 10 M / 40 M, four-sample adaptation | 24 | 3,567 | 104,038 | 85,608 |
| 20 M / 80 M, same adaptation | 12 | 3,935 | 114,771 | 47,220 |
| 80/3 M / 80 M, optimistic three-sample adaptation | 9 | 5,246.67 | 153,028 | 47,220 |

The higher-rate rows fail before other fill work. Even shortening the 20 M
pulse from three triple-parts to two removes only about six instructions:
roughly 23.17 cycles/symbol, still over its 12-cycle total budget. A different
block/paged axis renderer might do better; this is not an impossibility claim
about every future factorization. Deeper queues cannot repair a fill deadline.

The unchanged encoder is another independent limit. For this exact pattern,
the mean frame budgets are 1,331,424 / 665,712 / 332,856 / 249,642 cycles at
5 / 10 / 20 / 26.67 M. The measured 499,197-cycle mean suggests about 25%
average encoder reserve at 10 M if work were unchanged, but exceeds the
20/26.67 M budgets. Individual 8PSK periods at 10 M are only 532,656 cycles,
below the observed live maximum: mode-specific averages, burst lead, cache
contention and real ingress must be measured. Queues absorb bursts, not a
sustained average production deficit.

## Where to spend the RAM

The conservative measured floor leaves **55,327 B above the 20 KB reserve**.
Keep a guard and respect the **31,744 B largest block**; do not request one
contiguous 50 KB allocation. The options below are future code changes, not
console settings in 479a068:

* **Jitter/ingress buffering:** three separately allocated maximum packed
  slots cost **50,016 B**, extending a two-slot queue to five. The measured
  minimum would leave about 25,311 B before allocator overhead/additional
  dips. `ring_t` allows eight slots, but the encoder's three-pointer array and
  cleanup need extending. More queued frames also increase ACM actuation
  latency; the present two slots already had zero underruns.
* **Encoder speed:** move the **17,696 B** PSRAM context internal and reserve
  a maximum **25,920 B** hot-table cache for this four-mode pattern. Combined
  cost **43,616 B** leaves about 31,711 B from the observed minimum, before
  overhead. Existing cache sizes for 4/11/13/17 are
  23,632/25,920/21,936/23,232 B. Prepare resets cache pointers, so caching must
  be reapplied after every mode change, with its copy cost profiled. Allocate
  or split buffers in an order that accommodates the largest block.
* **Real ingress:** preserve reserve for SPI/USB DMA slots and backpressure
  instead of filling all free memory speculatively. This board run used
  synthetic payload; host tests cover in-band controls and split GCS reads.

These savings belong to the **5 M layout**, not an unconditional 50 KB credit
at every rate. Promising separate higher-rate experiments are:

* **10 M:** retain the fast **32,768 B full complex table**, with QPSK rendered
  as its subset. Its renderer-only estimate is 32,698 cycles/bank, comfortably
  below 85,608. Moving the maximum context to PSRAM, as demonstrated at 5 M,
  reduces the old 59,744 B internal allocation requests to **42,048 B**.
  Against this run's heap baseline that suggests about 33 KB minimum remaining,
  but requires a fresh layout/timing measurement. This is the most direct
  next mixed-VCM experiment; it uses the full table rather than the axis loop.
* **20 M:** use the existing **16,384 B six-symbol 8PSK complex table** as a
  common QPSK/8PSK pulse, rather than keeping two native pulse pipelines.
  Its eight-phase kernel issues about **8 + 1/24 cycles/symbol**, or 31,644
  cycles/fresh bank against 47,220. RAM can then pay for a resident encoder
  and hot data. QPSK would use a different pulse from its current fixed mode;
  implement and validate a new common-pulse oracle while retaining fixed-mode
  golden CRCs. Encoder/cache speed and boundary lookahead remain unqualified.
* **26.67 M:** the existing **36,864 B five-symbol 8PSK table** also contains
  QPSK's points. Reusing its four-symbol descriptor renderer is a better
  starting point than the per-symbol axis loop. It needs variable frame
  extents, persistent grouping across both modes, a maximum FEC context,
  mode-specific resident caches and revised queue admission. A QPSK frame
  needs **66** 512-symbol pages versus 44 for 8PSK; the existing 54-page queue
  cannot hold that full frame plus reserve. Pulse and rounding differ from
  native fixed QPSK. No current code or timing result establishes mixed VCM.

## Merge recommendation

Merge the **5 M opt-in axis path**, its tests and diagnostics, plus this saved-
dump verifier and evidence. Keep `vcm axes off` as the default, fixed-mode
goldens unchanged, and the 5 M rate guard. The supplied board run proves the
4/11/13/17 synthetic radio-off case; the host verifies all 14 MODCODs. Keep
deeper queues, cache migration and higher rates in separate experiments.
The conventional VCM branch can coexist with this choice; do not silently
replace its allocator or pulse selection while integrating.

Coordinator console commands, radio off, to reproduce the measured case:

```text
spiinput off
vcm axes on
vcm 1 4 11 13 17
wifimem 4 5m
dvbdump 4 5m 64
dvbpace 4 5m 30
vcm off
vcm axes off
```

Use binary-aware capture for the dump. Retain the >=20 KB minimum heap floor,
zero overlap/late/fatal/underrun counters and >=256 B encoder stack reserve.
No new firmware rate is enabled by this assessment.
