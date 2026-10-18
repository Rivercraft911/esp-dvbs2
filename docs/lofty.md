# 8PSK at 80/3 Msym/s

The 54/50/46-page queues stream partially queued PLFRAMEs using a measured
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

## Admission clock fix

In daec325, a new worst page gap recalculated admission inside the publication
loop. Its cold calculation became part of the next measured page gap. The
permanent maximum then raised its own thresholds. Supplied round-11 clean
records end at 41/42 pages on every mode, with gaps around 14,440 cycles.

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

## Offline checks and collection

The producer test checks FEC ordering, exact FEC bytes, a cold admission path,
real rate violations and cycle-counter wrap. The threshold formula is compared
with every missing-page deadline over 11,766 gap/wake combinations plus limiting
cases. Event-clock tests run the real publisher and consumer for 1,000 frames
per mode with minimum-prefix and full-queue starts. Baselines have no steady
dummies; injected FEC delays use fallback without queue faults. These timing
inputs are model assumptions, not new measurements.

No-dummy CRCs remain 2061900610/1873478180/233370528. All QPSK CRCs and the
200-frame wrap check remain unchanged. Forced-dummy dumps match every Python
DAC word and LeanDVB recovers contiguous payload without errors or corrections.
Physical ID/EOF/dummy-return tests check vector alignment and page-wake policy.
Xtensa tests add 99 guarded page batches with poisoned metadata and wrapping
release counters to the existing 4,320 slices, mapper and ISR tests.

After the coordinator builds/flashes the revision with N0CALL:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round12
```

Each mode gets 20 s clean and two 5 s observed runs, about one minute including
IDs. The runner never flashes or transmits; `--plan` opens no port. Clean gates
remain ESP_OK, complete playback, zero late fills/switches and queue faults,
fill <=40,137 cycles, Wi-Fi minimum >=20,000 B and stack >=512 B. Startup dummies
are allowed; steady dummies and lead are reported. QPSK's mean/lead gates remain
unchanged. Only the coordinator can establish the hardware result.
