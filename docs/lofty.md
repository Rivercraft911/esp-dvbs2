# 8PSK at 80/3 Msym/s

Queues of up to 54 pages stream partially queued PLFRAMEs using a measured
production bound. If admission fails, the fill selects standard dummy PLFRAMEs.
It never waits for the producer. QPSK's kernels and queue policy are unchanged.
This candidate needs the coordinator's hardware qualification; no board or radio
was used during development.

## FEC and page ownership

The producer already runs payload, BB, BCH and LDPC before waiting for queue
pages. There is no frame-wide page reservation. While data frame k plays, the
workspace can contain the complete FEC for k+1, even with zero spare pages.
`tests/test_lofty_producer.py` holds the actual producer at its full-queue wait
and compares that workspace against the independent C encoder. It checks all
three normal geometries and a deliberately full 44-page queue. BB and LDPC
still run with that queue full. The supplied daec325 records also have one more
LDPC call than completed encoded frames on each mode.

Mapped words and LDPC temporary storage share `dvbs2_fast` scratch. A further
FEC frame cannot overwrite an incompletely published mapped frame. This change
keeps the existing one-frame overlap, without another frame buffer or kernel
changes.

## Admission accounting

The round-12 silicon records for ebf8fb4 still end at 41 active / 43 waiting
pages, with G=13,268/13,268/13,748 cycles for 13/15/17. The accounting change
did not solve steady dummies. The source of that gap is unknown. The host
reproducer below demonstrates a feedback path, not the measured cause.

In daec325, a new worst page gap recalculated admission inside the publication
loop. The time spent there could enter the next measured gap and permanently
raise admission. Round-11 clean records end at 41/42 pages, around 14,440 cycles.

The host reproducer injects a 13,500-cycle cold calculation into a producer
whose pages take 1,000 cycles. The old code learns 14,500 cycles and 41/42 pages.
Threshold calculation now happens after EOF publication, outside the recurring
page intervals. The same test retains 1,000 cycles. Real injected page stalls
still raise the bound and its miss counter. `admission_update_max_cycles`
reports the separate calculation cost; encoder work timing still includes it.

A normal frame has 22,194 symbols at three samples/symbol: 66,582 samples,
or 199,746 CPU cycles at 240 MHz and 80 MS/s. Each full page has 128 four-symbol
descriptors, or 1,536 samples. Frames alternate between:

- 43 full pages plus a 534-sample tail;
- a 1,530-sample first page, 42 full pages and a 540-sample tail.

Both need 44 pages. The first two pages perform initial and bulk mapping.
Only later publication gaps enter the recurring production measurement G.
Full-queue sleep is subtracted; interrupts and runnable-task delays remain.
The first queued frame calibrates G before playback. Use planning bound W=2G.

The renderer issues at least 23 instructions per four-symbol descriptor.
Thus a full page cannot be consumed in fewer than C=128*23=2,944 CPU cycles.
This lower bound follows the executed instruction trace and the single-issue
rules in ESP32-S3 TRM v1.8 section 1.7, Table 1.7-2. Splitting a page across
callbacks renders at least the same descriptors. Every page before EOF is full,
including the skipped two-symbol prefix.

Let P be queued pages and k=1..44-P a missing page. Reserve R=9 pages,
at least (9*1536-6)/3=4,606 symbols while current-frame production is unfinished:

```
D + k*W <= (P-R+k-1)*C       [CPU cycles]
```

D=0 while the producer is mapping, or one 15,740-sample bank period,
47,220 cycles, while waiting to resume. Producer-owned `producer_phase`
distinguishes these states. The notification flag cannot: the consumer clears
it before the producer gets CPU time. A new full-queue wait rechecks space after
arming, so it cannot sleep on an already released page.

The inequality is linear in k; only its first and last cases need checking.
For G=1,000 cycles, admission needs 10 active / 26 sleeping pages. G=0 in the
synthetic-clock fixture gives 9/26. Slower measured production raises thresholds
up to 44. Complete frames always qualify. Lagging completed-frame counters and
32-bit counter wraps are covered by tests. Dummy selection also wakes obsolete
full-queue waits, so neither side waits for the other to advance.

This guarantee is conditional on W and D, rather than arbitrary CPU starvation.
The record reports G, final active/sleeping thresholds, partial admissions and
new worst live gaps that exceed the previous 2G. Updates happen after each frame
whose maximum grew. Following-frame FEC can lower lead after EOF is produced;
the reserve does not establish an unconditional lead floor.

## Queue-depth experiment

The aaeb50e sweep on 17 (radio off, Wi-Fi off, 10 s per count,
`notes/eight-lofty-round16/`) measured:

| Pages | Data frames | Steady dummies | Worst fill, cycles |
| --- | --- | --- | --- |
| 46 | 9,374 | 17,602 | 39,018 |
| 50 | 11,238 | 5,180 | 39,007 |
| 54 | 12,015 | 0 | 38,551 |
| 58 | 12,015 | 0 | 38,509 |

This establishes 54 pages for that measured 17 case. It does not yet qualify
Wi-Fi-up or keyed playback. The caps for 13 and 15 are also 54; 13 previously
ran with that depth, while 15's new depth needs the gate below.

`dvbpace 17 26m667 10 128 0 pages=58 nowifi` selects an explicit internal
queue and the existing radio-off timer without starting Wi-Fi. `pages=44..64`
is accepted only for 8PSK 26m667. With `nowifi`, omitting it retains 54/50/46
pages for 13/15/17. Omitting `nowifi` retains Wi-Fi-up pacing, now with automatic
queue sizing when no explicit `pages` override is given. Neither flag changes
admission, bank-end wakes, producer work or the renderer. `nowifi` excludes
the interrupt observer, whose hooks belong to the Wi-Fi-up worker. No trace
or observer is enabled by the sweep.

A `lofty_page` is 528 bytes; 38 pages reside in the existing store. For
46/50/54/58 pages, the heap allocation is `(pages-38)*528` =
4,224/6,336/8,448/10,560 bytes. The largest requested queue costs 6,336 bytes
more than default 17. Allocation failure returns an error with cleanup;
it never silently reduces the requested queue.

The coordinator runs all four counts for 10 seconds each, records data frames,
startup/steady/alignment dummies, fill timing, actual pages and internal heap,
and keeps going after failures:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round16 --page-sweep
```

Use `--plan` to inspect commands without opening a port. This is a diagnostic
sweep, not the Wi-Fi-up 20-second qualification. It requires complete runs
without late fills or queue faults, and reports fill maxima without applying
the separate 40,137-cycle qualification ceiling. Compare dummy counts only
among complete runs. Falling counts support a queue-depth/burst interaction;
unchanged counts would direct the next measurement toward admission/wakes.

## Internal RAM and automatic queues

The repeated pace reference keeps its store prefix but moves its heap tail
to mandatory PSRAM. The encoder still copies the same bytes, in 1,316-byte
chunks, into its existing internal payload scratch. Keyed runs have no
reference allocation and retain the original contiguous xorshift stream.
The input and encoder CRC checks cover both layouts. PSRAM-copy timing and
its shared-cache effects still require silicon measurement.

The new reference tails are 5,066/6,416/6,960 B for 13/15/17, with a 304 B
prefix in the store. The queue-budget fields share that store's unused tail;
they add no static internal allocation. All 38 resident queue pages, the
8,144-byte encoder workspace and reference prefix remain shared with the
existing 64 KiB store. The 36,864-byte renderer table and 2,560-byte timed
overlap cache stay internal because the fill uses them. Dummy samples and
split BCH pair tables already use PSRAM. Moving these hot fill tables or
encoder caches is not needed to reach the 54-page cap.

Wi-Fi start-up now calls `radio_s3_stream_ready` once before any timed fill or
TX setup. The encoder has its task, stack, workspace and input driver ready,
then sleeps on a notification without publishing. The hook allocates the
final queue and wakes the producer, and waits with `vTaskDelay(1)` until it
is full. No live queue is resized or copied. The Wi-Fi-only `wifimem` probe
uses this same keyed-layout hook and returns before RF setup. Explicit-page
diagnostics, Wi-Fi-off pace, raw dumps and QPSK keep their existing setup.

For actual post-Wi-Fi free 8-bit internal heap F, page size S=528 B and
38 resident pages, the requested count is:

```
P = min(54, 38 + floor((F - 20000 - 2048 - 64) / 528))  [pages]
```

The 20,000 B floor is the user's gate. The 2,048 B guard exceeds the 1,816 B
free-up versus minimum dip in the round-12 first 13 run. The 64 B allowance
covers aligned allocator overhead; actual free heap is checked again after
allocation. Fragmentation can lower P, one page at a time. Fewer than 44
pages, an already violated startup minimum, or insufficient remaining reserve
returns an error before playback. Both pace and keyed setup cap at 54 pages
for each of 13/15/17. Actual selected counts and the floor still need the board.

`queue_sizing` reports automatic/ready, target, floor/guard, free before and
after allocation, and the pre-allocation minimum. `reference_psram_bytes`
and `reference_store_bytes` report the fixture footprint. `wifi_free_up`
retains its meaning before the queue hook; `wifi_min_free` now refreshes after
playback and cleanup, including the final queue allocation. Historical records
only sampled that field at Wi-Fi startup.

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round17 --queue-gate
```

This collects keyed-layout `wifimem` and 20-second clean Wi-Fi-up `dvbpace`
records for 13/15/17, without TX, trace or observer. It reports pages for both
setups, retains faults and continues through all modes. The gate requires
54 pages, ESP_OK, full playback, no late banks or queue faults, fill at most
40,137 cycles, zero steady dummies and Wi-Fi minimum at least 20,000 B.
Startup dummies are reported and allowed. No keyed playback is performed.

## Fill and dummy waveform

Consecutive full pages now share vector setup. The kernel releases each page
with a memory barrier after its last descriptor. Batches stop before EOF,
physical queue wrap and the resident/heap split, and retain the nine-page
reserve. Diagnostic page-wake mode uses one page per call and keeps its original
notification policy. Default wakes remain at bank end.

Lead observations use an initial publication snapshot, conservatively ignoring
new production during a batch. The minimum stays in samples until reporting.
Its 32-bit accumulator borrows the unused prefetched third splice word; that
word is never rendered. This keeps the new kernel and control code within the
same internal heap boundary as daec325. Tables, queue pages, input allocation,
640-word timed cache and 1,792-byte encoder stack are unchanged. Stack-size
reporting now matches the allocation, rather than printing the old 2,048 bytes.
Raw dumps retain their 768-word initial overlap.

[EN 302 307-1 V1.4.1, clauses 5.5.1 through 5.5.4](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf)
defines MODCOD 0, a 90-symbol header and 36 pilot-less slots. Each dummy has
3,330 symbols. Pairs preserve the four-symbol rounding and aligned stores,
since 3330 mod 4=2 and 6660 mod 4=0. No payload bytes are skipped or repeated.
Startup counts cover choices before three data frames finish; later choices
are steady-state dummies, with target zero. All valid source transitions remain
in IRAM.

## Optional queue trace

`dvbpace ... trace` is capture-only; `observe` and `trace` are mutually
exclusive. It requests no extra memory during stream, task or Wi-Fi startup.
The request flag cannot change the fill's path or allocate memory before
64 data frames have played. Admission, encoder kernels, queue size, input,
workspace and producer warm-up are unchanged.

The supplied 964aa73 records for 15 already have identical 50-page queues,
one-frame prefill, end-run free internal heap (44,135 B), largest internal block
(9,728 B), and stream allocation addresses. The ring already requested PSRAM.
The Wi-Fi minimum is lower for trace (18,635 vs 20,451 B). These measurements do
not establish why the old traced run failed; fewer pages/internal ring storage
are not supported by those records.

The producer now allocates the 491,600-byte ring at frame 66, exclusively in
PSRAM, and checks `esp_ptr_external_ram`. A frame publishes 44 pages. With at
most 64 pages queued, 66 produced frames imply at least 66*44-64=2,840 released
pages, beyond the 2,816 pages of 64 complete frames. Dummies release no pages.
This page accounting establishes the allocation point without timing assumptions.

Only the small header is initialized. Record storage stays untouched until its
event writes; there is no half-megabyte zero-fill. Heap poisoning is disabled in
the firmware configuration. Both cores capture data IDs 68 through 131, zero
based, after allocation. The producer's first event snapshots the learned
admission bound. Dummies do not advance the data ID. No recorder events or
clock reads occur outside that 64-frame window.

`trace_setup` reports waiting/allocated/allocation_failed, allocation frame 66
and its derived minimum of 64 played frames. `stream_trace` adds the actual
external address, producer/consumer counters and page count at allocation,
plus before/after free 8-bit internal and PSRAM heap. Those heap snapshots may
also include concurrent system activity; the mandatory capability and external
address check forbid an internal-memory fallback. Early failure before frame 66
reports no allocation; allocation failure leaves the stream running and reports
an incomplete capture after playback.

Each active event reads CCOUNT once and stores six 32-bit words: timestamp,
packed frame/event tag, produced, consumed, a and b. Its cursor stays internal;
only the record goes to PSRAM. The bounded ring reports overflow instead of
overwriting earlier evidence. The 50-byte IRAM writer has no function calls,
clock conversion, dummy counting, per-event cost accounting or printing.
These are source/disassembly facts, not a silicon overhead measurement.

The pace record's `stream_trace` version 2 still prints rows as
`[us, cycles, frame, event, produced, consumed, a, b]`. Conversion and dummy
counting happen after playback. At arming, each owning core brackets a shared
`esp_timer_get_time()` read with CCOUNT reads. Those three values are printed
as its `anchor`. With fixed 240 MHz and power management disabled in
`firmware/sdkconfig`, conversion is:

```
base_cycles = before + (after-before)/2                 [CPU cycles]
us = anchor.us - epoch_us + uint32(cycles-base_cycles)/240  [microseconds]
```

`epoch_us` is the earlier armed anchor. Subtractions wrap modulo 2^32; capture
must fit within one 17.9 s CCOUNT period. The coordinator's runs are 10 s.
The anchor's before/after span and the shared clock's 1 us resolution expose
alignment uncertainty. Core-local CCOUNT values must not be compared directly
across cores. IDF v6.1 uses SYSTIMER for the shared clock
(`components/esp_timer/src/esp_timer_impl_systimer.c`). Sequence counters also
wrap modulo 2^32.

| Event | a | b |
| --- | --- | --- |
| frame_start (13) | 0 | 0 |
| fec_start/end (1/2), BB through LDPC | 0 | 0 |
| map_wait_start/end (3/4), pl_wait_start/end (5/6) | physical page index | mapper symbol position |
| publish (7) | physical page index | mapper symbol position |
| admit (8) | packed decision, below | dummies before this data frame, on data admission |
| release (9) | physical page index | data-frame sample position after release |
| bound (10) | learned G, CPU cycles | active threshold + 256 * waiting threshold |
| data_end (11) | cumulative dummy count | 0 |
| gap_max (12) | new maximum G, CPU cycles | mapper symbol position |

Admission packs the actual decision snapshot: bits 0..7 queued pages, 8..15
selected threshold, 16 completed frame available, 17 producer outside mapping,
18 forced second dummy for alignment, 19 dummy selected, 20..22 producer phase
(1 FEC, 2 map, 3 wait, 4 PL). Row counters are re-read when the event is logged
and may be newer than the packed snapshot. Publication/release markers follow
their atomic stores; they timestamp the logging point rather than the exact
store instruction. Wait markers surround the page wait, including its
notification recheck.

During these 64 frames only, trace uses one page per render call so every
release has a marker. `single_page_batches` records that diagnostic cost.
Before and after capture, default batching remains. Bank-end wakes, admission
and queue geometry are retained. There are no per-page kernel notifications.
Capture still perturbs timing, even without the interrupt observer, and is not
a clean timing qualification. All printing happens after playback stops.
Dropped events or an incomplete window produce `complete:false`; raw records
remain available. Unarmed lanes have `armed:false` and empty rows.

The supplied 16a03b4 runs (`notes/eight-lofty-round15/`) reach capture. Their
ring addresses are external and allocation changes free internal heap by zero
bytes. Active capture still misses the bank deadline: 54,722/50,730 cycles for
15/17. It forces single-page render calls and writes PSRAM from both cores;
these recordings do not qualify clean fill timing.

The partial traces contain eight/four complete data frames for 15/17. All
352/176 publication/release pairs match. Every page-wait start finds the queue
full, at 50/46 pages. Before the final failed bank, the longest recorded waits
are 201.4/223.4 microseconds; exclude the final shutdown wait, which ends after
the fill has stopped. Frame 68 of 17 spends 444.2 microseconds in page waits
and 298.8 microseconds in FEC. At its admission, 42 pages are queued against
the 43-page waiting threshold, so one dummy is selected. The frame is complete
at the next decision, but alignment requires another dummy. Frame 69 repeats
with 38 pages against 43. Both learned bounds are already about 14,670 cycles
before capture, giving 41 active / 43 waiting pages. These are measured
backpressure and admission decisions under instrumentation, not evidence of
lost pages or a clean-run diagnosis. The trace-free depth sweep isolates the
next variable. No board or radio was accessed during development.

## Offline checks and collection

The current trace checks use the real concurrent producer and fill. They
verify identical startup allocation counts/sizes/capabilities and prefill with
trace on/off, allocation only after 64 played frames, 64 complete captured frames
per mode, all 2,816 matching publication/release pairs and unchanged waveform
checksums. A poisoned buffer proves only the header is initialized. Deferred
allocation failure leaves the waveform unchanged.
Recorder units check one timestamp per event, two shared-clock reads total,
CCOUNT/epoch wrap, allocation failure and explicit overflow. Existing admission
timing models do not establish capture timing on silicon.

No-dummy CRCs remain 2061900610/1873478180/233370528. All QPSK CRCs and the
200-frame wrap check remain unchanged. Forced-dummy dumps match every Python
DAC word and LeanDVB recovers contiguous payload without errors or corrections.
Physical ID/EOF/dummy-return tests check vector alignment and page-wake policy.
Xtensa tests add 99 guarded page batches with poisoned metadata and wrapping
release counters to the existing 4,320 slices, mapper and ISR tests.

After the coordinator builds/flashes the revision with N0CALL:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round15 --trace
```

Trace collection runs only 15 and 17 at 26m667, 10 s each, without the interrupt
observer. `tests/test_stream_trace.py` checks 64 real threaded frames
per mode and two-frame startup-only runs for 15/17, with no trace allocation
or recorder calls and the same stream allocation footprint.
It checks forced dummies and equal output checksums with tracing on/off.
No timing model is used in this check.

Without `--trace`, `--page-sweep` or `--queue-gate`, each mode gets 20 s clean and two 5 s observed runs.
The runner never flashes or transmits; `--plan` opens no port. Clean gates
remain ESP_OK, complete playback, zero late fills/switches and queue faults,
fill <=40,137 cycles, Wi-Fi minimum >=20,000 B and stack >=512 B. Startup dummies
are allowed; steady dummies and lead are reported. QPSK's mean/lead gates remain
unchanged. Only the coordinator can establish the hardware result.
