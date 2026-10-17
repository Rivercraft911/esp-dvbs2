# Fill and frame rings

Core 1 fills the inactive DAC bank from continuously encoded frames. It reuses
finished overlap samples, reads only fresh symbols and pulse shapes them with
internal lookup tables. No per-symbol PSRAM traffic occurs at the fast rates.
`dvbtx.c` owns the lifecycle; `ring.c` owns frame publication and release.

## Rate selection

A token names a symbol rate, not an experiment. `26m667` means exactly
80/3 Msym/s; the integer `symbol_rate` telemetry truncates that value.

| Token | Modulation | DAC, MS/s | Samples/symbol | Pulse, symbols | Ring |
|---|---|---:|---:|---:|---|
| omitted | QPSK | 40 | 40 | 9 | 8 raw frames in PSRAM |
| `5m` | QPSK / 8PSK | 40 | 8 | 9 | 2 paired frames |
| `10m` | QPSK / 8PSK | 40 | 4 | 9 | 3 paired frames |
| `20m` | QPSK | 80 | 4 | 9 | 3 quad frames |
| `20m` | 8PSK | 80 | 4 | 6 | 3 paired frames |
| `26m667` | QPSK | 80 | 3 | 9 | 2 quad frames |
| `26m667` | 8PSK, experimental | 80 | 3 | 5 | 36 offset pages |

The `33m333` QPSK sign queue is in [moonshot.md](moonshot.md). The new 8PSK
`26m667` offset queue, model limits and pending board gates are in [lofty.md](lofty.md).

`20m` adopts the previously measured `20mq4` and `20ms6` kernels. QPSK 9/10
whole-bank maxima were 36,766 cycles with q4 versus 45,852 with the old
paired path. The six-symbol 8PSK 5/6 fill measured 40,546 cycles versus
45,318 for nine symbols. These are separate recorded runs, not a same-session
A/B claim. [QPSK](../results/2026-10-17-qpsk-goal/runs.jsonl),
[paired QPSK](../results/2026-10-17-next5/runs.jsonl),
[short 8PSK](../results/2026-10-17-next4/README.md),
[nine-symbol 8PSK](../results/2026-10-16-20msym/README.md).

`10m80` duplicated a symbol rate with twice the DAC work per second. Its
8-sample waveform remains available at 5 Msym/s. `10m` retains the 40 MS/s
schedule, resident workspace and original waveform. The three experiment
tokens are removed. The consolidated 20 Msym/s defaults require the coordinator's
radio-off check; the six-symbol waveform has not been qualified by keyed RF.

## Tables and samples

For a nine-symbol pulse h, phase k has integer I/Q coordinates L[k] in
2^-30 units. Three-symbol windows independently contribute

```
T[j,w,p] = round(sum(a=0..2, L[w[a]] * h[(3*j+a)*sps+p]) / 2^38)
y[p] = T[0,new,p] + T[1,previous,p] + T[2,oldest,p]
```

Taps have units 1/256 DAC LSB; results have DAC LSB units. Per-axis table
biases sum to 512 and keep every field nonnegative, so 32-bit additions do not
carry from I into Q. XOR `0x80200` produces signed I[9:0], Q[19:10]. The
six-symbol pulse uses two triples instead of three. `host/shaper.py` derives
the root-raised-cosine pulse, roll-off 0.35, with inverse sample-hold correction
(EN 302 307-1, 5.6 and Annex A). Firmware truncates scaled taps before building
tables. The six-symbol oracle derives its constants independently.

The QPSK block kernels use three four-symbol windows, 256 rows each. A row
contains four output symbols, so table size is
`3 * 256 * 4 * sps * 4 B = 12288 * sps B`: 36,864 B at 3 sps and 49,152 B
at 4 sps. `host/qpsk_fill.py` checks these tables against convolution and bounds
rounding error by 1.5 DAC LSB per axis. Block boundaries follow the global
symbol timeline: a 33,282-symbol frame ends halfway through a block.

The fill never reads the bank owned by the radio. A private circular cache
retains 2,176 words at 40 MS/s or 768 at 80 MS/s: `BANK - STEP + LEAD`, including
the first bank's extra lead. Physical wraps split a fill into two callbacks;
only the cache suffix surviving the second callback is saved. Fresh ring reads
move forward, retaining eight history symbols plus two edge symbols. Quad
reads retain their global four-symbol phase and enough history across frames.

Publication/release uses acquire/release atomics. A slot remains owned until
the consumer no longer needs its symbols. Slot release notifies the fast
encoder; default QPSK polls. On starvation, a 3,330-symbol DVB-S2 dummy frame
preserves a valid stream without skipping payload (5.1.5/5.5.1). Re-reading an
overlap must repeat the same real/dummy choice. Station ID pauses the payload
clock while the DAC schedule continues.

## Internal RAM

One 65,536 B static store is shared between shaping tables and command-owned
storage. Two 65,536 B physical DAC banks are reserved separately. Packed frame
allocations include 16 readable guard bytes and round up to 16 B.

| Mode | Table, B | Rest of store | Separate internal ring allocation |
|---|---:|---|---:|
| QPSK 5m | 8,192 | 2 frames, dummy | 0 B |
| QPSK 10m | 4,096 | 3 frames, dummy | 0 B |
| 8PSK 5m | 65,536 | none | 23,936 B: 2 frames + dummy |
| 8PSK 10m | 32,768 | 2 frames, dummy, BCH where it fits | 11,120 B third frame |
| 8PSK 20m | 16,384 | 3 frames, dummy, resident BCH | 0 B |
| QPSK 20m | 49,152 | 12,432 B workspace reservation + 864 B dummy | 25,056 B for 3 frames |
| QPSK 26m667 | 36,864 | 16,704 B for 2 frames + 864 B dummy + 8,144 B workspace + window | 0 B |

All packed payloads borrow encoder scratch until BB consumes them. QPSK
26m667 uses a fast-only workspace allocation; no plain stage may use it.
[33m333 queue layout](moonshot.md). Default QPSK keeps its original PSRAM allocation order. Internal workspaces
lend their unused tail to the fill window. Cleanup must free only separately
allocated storage. Context/BCH policy is in [encoder.md](encoder.md). These
are requested sizes, not free-heap predictions. Wi-Fi allocation/alignment and
instruction placement require a board measurement.

## Correctness and diagnostics

`tests/test_stream.py` runs the actual startup, encoder, ring and fill with
host task stubs. Every DAC word matches the plain encoder plus C shaper oracle;
every allocation is failed in turn, and repeated commands must leave no leaks.
It checks complete physical-wrap cycles at both geometries. Host clocks and
heap counters are synthetic and provide no performance evidence.

`tests/data/waveform_crcs.json` retains 57 host-generated dumps from main `46f90af`,
using its opt-in kernels for the adopted 20m waveforms. Keys are
`MODCOD/sps/DAC-MHz/frames`, stored callsign N0CALL, peak 128. Decimation is
10 for default QPSK, 2 for QPSK at 8 sps, otherwise 1. All original surviving
waveform CRCs remain exact. Changed `20m` examples:

| MODCOD | Frames | Previous 20m CRC | Adopted 20m CRC |
|---|---:|---:|---:|
| 4 | 40 | 2,594,131,925 | 4,107,112,128 |
| 11 | 40 | 2,779,934,507 | 1,940,267,510 |
| 13 | 40 | 474,263,962 | 3,178,964,763 |
| 15 | 40 | 1,757,765,864 | 4,256,019,779 |

For 16-frame QPSK 20m dumps, CRCs are 3,789,273,049 (4) and 2,081,653,259
(11). For 16-frame short 8PSK they are 3,479,114,506 (13) and 68,643,998 (15).
These are host checks against the old source, not new board measurements.

The 58th fixture is a 16-frame 8PSK 9/10 `20m` dump, CRC 3,782,178,445,
checked on the host with independent decode. Its board check is pending.

`profile.encoder` reports payload/BB/BCH/LDPC/map/PL calls, cycles and cache
requests/misses. `profile.fill` reports copy/whole/partial/save/notify.
`shaped_words + reused_words == fill_words`. `worst_call` describes one callback;
`max_fill_cycles` describes a complete bank. Never add unrelated phase maxima.
Timed fills read CCOUNT only; untimed dumps also read cache counters. Encoder
stages read both. `host/stream_profile.py` summarizes records; its reported
15% reserve test is a planning target, separate from cleanup's 2% regression gates.
