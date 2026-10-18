# 8PSK at 80/3 Msym/s

Data admission now streams a partially queued frame once measured page production
can cover the remaining playback. Complete frames always qualify. Otherwise the
fill selects standard dummy PLFRAMEs and rechecks at boundaries. Queue sizes stay
54/50/46 pages for MODCODs 13/15/17. QPSK's encoder, renderer, stack and cache policy
are unchanged. This candidate has offline checks only; no board or radio was used.

## Admission derivation

A normal frame with pilots is 22,194 symbols, or 66,582 samples at three
samples/symbol. At 240 MHz and 80 MS/s it lasts 199,746 CPU cycles.
A page contains 128 four-symbol descriptors, normally 1,536 samples. Frames
alternate between zero- and two-symbol prefixes:

- zero prefix: 43 full pages and a 534-sample tail;
- two-symbol prefix: a 1,530-sample first page, 42 full pages and a 540-sample tail.

Both frames need 44 pages. EOF is published immediately. Metadata occupies
existing padding; pages stay 528 B. Sequence counters use release/acquire
ownership and wrap independently of physical page indices.

The first page precedes bulk mapping; the second completes it. Supplied round-10
records have preparation maxima of 43,595/44,262/44,762 cycles in the observed
13/15/17 runs. These cannot be used as a recurring page cost. Once two pages
exist, later pages only prepare descriptors from the mapped frame. The producer
now records their maximum publication gap G, excluding full-queue sleep but
including preemption and publication overhead. The first queued frame calibrates
G before playback. Every larger observation raises the planning bound W = 2G.

The renderer's two-block body has 47 single-issue instructions. Each full page
therefore needs at least C = 128*23 = 2,944 issue cycles, ignoring additional
loop, memory, call and interrupt costs. `host/lofty_cycles.py` checks this lower
bound against the executed Xtensa instruction trace. See ESP32-S3 TRM v1.8,
section 1.7, Table 1.7-2. Splitting pages across callbacks renders at least the
same descriptors. All pages before EOF are full, including the skipped prefix.
This bounds consumption during the fill burst as well as between banks.

Let P be queued pages, R = 6 reserve pages, and k = 1..44-P a missing page.
Six full pages retain at least (6*1536-6)/3 = 3,070 symbols. Require

```
D + k*W <= (P-R+k-1)*C       [CPU cycles]
```

D is zero while the producer is actively mapping, or one bank period,
3*15,740 = 47,220 cycles, while awaiting a bank-end wake. Producer-owned
`producer_phase` distinguishes them. The notification's `waiting` flag cannot:
the consumer clears that flag before the producer actually resumes. A producer
about to sleep must recheck the newly released space before blocking.

Both sides of the inequality are linear in k, so only k=1 and k=44-P need
checking. The producer computes both thresholds when G grows; the fill reads
one word and selects a threshold at each boundary. For G=1,000 cycles, thresholds
are 7 active pages and 23 sleeping pages. Slower measured production raises them,
up to complete-frame admission. G=0 in the synthetic-clock host fixture gives
6/23. The first two expensive pages are necessarily behind every threshold.
A lagging completed-frame counter is handled as incomplete, including across
counter wrap; it cannot accidentally admit an empty queue.

The reserve covers unfinished current-frame production. After EOF has been
produced, following-frame FEC can still lower queue lead; silicon must measure
that minimum. In the baseline event model the minima are 3,248/2,224/3,584
symbols for 13/15/17, so the model does not establish a universal 3,000-symbol
lead floor.

This is a conditional timing guarantee under W and D, not a bound on arbitrary
CPU starvation. `queue_stream_page_max_cycles` reports G;
`queue_stream_bound_misses` counts live gaps exceeding the previous 2G;
`admission_pages`/`admission_wait_pages` report the final thresholds;
`streaming_admissions` counts partial-frame admissions. The coordinator must
confirm these bounds, zero queue faults, dummy airtime and lead on silicon.
The consumer never waits. A dummy decision wakes any obsolete full-queue sleep,
so neither side waits for the other to advance.

## Dummies and rendering

[EN 302 307-1 V1.4.1, clauses 5.5.1 through 5.5.4](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf)
defines MODCOD 0, a 90-symbol header and 36 pilot-less slots. Each dummy has
3,330 symbols, or 9,990 samples. Data bytes are neither skipped nor repeated.

Pairs remain deliberate: 3330 mod 4 = 2, whereas 6660 mod 4 = 0. One dummy
changes the four-symbol rounding groups and misaligns queued vector stores by
eight bytes. Removing pairs requires re-grouping descriptors or a slower store
kernel. This candidate first removes the full-frame admission serialization.
`dummy_alignment` still counts the second dummy of each pair. `dummy_startup`
counts selections before three data frames finish; `dummy_steady` counts later
selections, with target zero. All source transitions remain in IRAM.

The renderer restores the full C table: 36,864 B instead of 36,352 B. Constant
C0-to-C1 spacing removes one instruction per twelve samples. The trace model
returns to 23.5 cycles/block from 24.5, saving 1,312 issue cycles per 15,740-sample
bank. Calls, cache/bus stalls and interrupts remain additional. Full queue pages
call the bulk renderer directly. Every page-entry lead observation is retained;
its minimum is converted to symbols once per callback.

Timed 8PSK starts with station ID. Its initial extra 128-word overlap never
reaches the data cache, so that cache needs only the regular 640 words instead
of 768. Raw dumps retain 768. This saves 512 B and funds the wider table.
Round 10 left at least 976 B of the 2,048-byte encoder stack unused. The 8PSK
stack is now 1,792 B; the existing 512-byte minimum gate remains. Linked static
RAM grows 256 B, input allocation grows 512 B, cache shrinks 512 B and stack
shrinks 256 B: no net paced SRAM increase. The store contains 36,864 B tables,
38*528 B pages, an 8,144 B workspace, 128 B mux and 336 B input prefix.
QPSK's allocation sizes and kernels are unchanged.

## Offline checks and collection

`tests/test_lofty_admission.py` compares the threshold formula with every
missing-page deadline over 11,766 gap/wake combinations. Its event clock then
runs the real publisher and consumer for 1,000 frames per mode, starting with
only seven pages, across sequence and queue wraps. Baseline model inputs yield
zero startup/steady dummies. Injected 180,000-cycle FEC delays cause fallback
dummies with no empty-queue faults. Tail-page times are assumed model inputs,
not new measurements. The separate ownership stress suspends the producer
indefinitely, uses the slow-rate complete-frame fallback, and checks 24,576
pages plus notification-before-block races.

No-dummy 16-frame CRCs stay 2061900610/1873478180/233370528; all eight QPSK CRCs
and its 200-frame wrap check stay unchanged. Forced dummy dumps match every
Python DAC word. LeanDVB recovers contiguous data with zero payload bit errors,
BCH corrections or LDPC flips. Physical ID/data and dummy/data transition tests
also cover the smaller timed cache and vector alignment.

After the coordinator builds/flashes the revision with N0CALL:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round11
```

Each mode gets 20 s clean, 5 s observed default and 5 s observed comparison,
about one minute including IDs. The runner never flashes or transmits;
`--plan` opens no port. Clean gates remain ESP_OK, complete playback, zero late
fills/switches and queue faults, fill <=40,137 cycles, Wi-Fi minimum >=20,000 B
and stack >=512 B. Startup dummies are allowed; steady dummies, production bounds
and minimum lead are reported. QPSK's mean/lead gates remain unchanged.
