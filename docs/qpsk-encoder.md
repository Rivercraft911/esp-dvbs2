# QPSK rates and packed encoding

QPSK now accepts `5m`, `10m`, `10m80` and `20m` in `dvb`, `dvbdump` and
`dvbpace`, for every supported QPSK MODCOD (4 through 11). The warm model
projects **151k to 186k cycles/frame**, including synthetic payload generation.
This is not a silicon result. The original 1 Msym/s command keeps its PSRAM
layout and its old timing. No board, serial port, Wi-Fi or radio was accessed.

| Selector | DAC rate | Symbol rate | Samples/symbol |
|---|---:|---:|---:|
| omitted, QPSK only | 40 MS/s | 1 Msym/s | 40 |
| `5m` | 40 MS/s | 5 Msym/s | 8 |
| `10m` | 40 MS/s | 10 Msym/s | 4 |
| `10m80` | 80 MS/s | 10 Msym/s | 8 |
| `20m` | 80 MS/s | 20 Msym/s | 4 |

8PSK still requires a selector. The callsign gate and station identification
apply to `dvb` at every rate. `wifimem 4 5m` and `wifimem 11 10m` now probe the
corresponding QPSK allocation; that command still accepts only the 40 MS/s
schedule. Allocation sizes are the same for a given samples/symbol at 80 MS/s.

## What changes

The fast QPSK stream uses the existing packed format: two canonical phases per
byte, low nibble first. QPSK data, headers, pilots and dummy frames all fit that
format. A frame occupies 16,672 B including alignment and the unpacker's read
guard, instead of 33,282 raw phase bytes. Two frames are kept at eight
samples/symbol and three at four. The existing notifications and finished-sample
cache now apply to those QPSK streams too.

QPSK has no bit interleaver (EN 302 307-1 V1.4.1, clause 5.3.3). The mapper's
existing bit-plane equations combine consecutive FEC bits with the prepared
Gold rotations. For FEC byte `x` and rotation byte `g`:

```
d = x XOR (x >> 1)
p1 = d XOR g
p2 = x XOR (g >> 1) XOR (d AND g)
index = (p1 AND 0x55) OR ((p2 AND 0x55) << 1)
```

A 256-entry, 1,024 B flash table maps each index to four phase nibbles in a
halfword. Eight PIE gathers and one vector unzip produce 32 packed symbols.
This avoids expanding phase bytes and then packing them again. QPSK's final
720-symbol pilot interval leaves 16 symbols after the whole vectors; four
scalar lookups write those final eight bytes. Pilots-off frames exercise the
same tail. Header and pilots were already prepared once by the 8PSK encoder.

The existing four/eight-sample shaping kernels are assembled with radix four
as well as radix eight. Their pipeline is the same. A QPSK three-symbol window
has `4^3 = 64` rows, so its table occupies 4,096/8,192 B at four/eight samples,
instead of 32,768/65,536 B for 8PSK. The packed ring, dummy and payload occupy
the rest of the existing 65,536 B `shaper_store`. The unpacker directly emits
`16 * (phase >> 1)` for these kernels. It adds three vector instructions per
32 symbols, rather than a scalar conversion per symbol.

The integration hook for the separate three-sample kernel is `fill_codes()`
in `shaper.c`, with `table.bits == 2`; its input contract is the compact QPSK
codes above. Enabling three samples also needs its taps/table builder, the
selector/validation, a nonzero default dump decimation, a larger `BANK_SYMBOLS`, and parity from `(first / sps) & 1`
in `fill()`. The present validation deliberately accepts only 4/8/40 samples.

## Cycles

At 240 MHz, a 33,282-symbol frame has the following one-core budgets:

```
cycles/frame = 240e6 cycles/s * 33282 symbols/frame / symbol_rate
20 Msym/s:       399384 cycles/frame; 85% = 339476.4 cycles/frame
80 MS/s / 3:     299538 cycles/frame; 85% = 254607.3 cycles/frame
9/10 payload:    7264 B/frame * 8 * (80e6/3) / 33282 = 46.562 Mb/s
```

The instruction trace model uses the same LX7 interlock rules as
[the 8PSK experiment](max-encoder.md). Sources are the
[ESP32-S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
1.7.1 and Table 1.7-2, pp. 65–69, and 1.7.3, p. 74. It retains register-window
spills and control transfers. It omits interrupts, cache misses and SRAM bus
contention. QEMU CCOUNT is not used as timing. The gather/unzip instructions
are specified in 1.8.37, p. 113 and 1.8.207, p. 290.

| MODCOD / rate | Model FEC cycles/frame | With estimated payload | With 25% allowance |
|---|---:|---:|---:|
| 4 / 1/2 | 118,283 | 150,834 | 188,542 |
| 5 / 3/5 | 127,244 | 166,360 | 207,950 |
| 6 / 2/3 | 124,304 | 167,829 | 209,786 |
| 7 / 3/4 | 129,611 | 178,575 | 223,218 |
| 8 / 4/5 | 132,759 | 185,005 | 231,257 |
| 9 / 5/6 | 131,783 | 186,250 | 232,813 |
| 10 / 8/9 | 120,795 | 178,942 | 223,677 |
| 11 / 9/10 | 122,419 | 181,295 | 226,619 |

Payload cost is estimated at `43525 / 5370 = 8.105 cycles/B`, from the internal
8PSK payload stage in [the 20 Msym/s run](../results/2026-10-16-20msym/runs.jsonl).
The model gives QPSK mapping 25,054 cycles/frame, versus the old stream's
measured roughly 331k; the old whole encoder takes 682k–685k cycles/frame.
The 25% allowance is a sensitivity calculation, not a measured upper bound.
For calibration, the same trace model gives 120,331 FEC cycles for unchanged
8PSK 2/3; adding its measured 43,525 payload cycles gives 163,856 cycles,
against the measured 167,031 cycles/frame.

The shaping specialization does not establish 15% fill margin. Using the
current measured 45.1k-cycle 8PSK bank as a reference, its additional unpack
instructions suggest about **45.5k cycles/bank** at four samples/symbol.
That is an estimate, not a measured QPSK fill. At 80 MS/s, `15740 words *
3 cycles/word = 47220 cycles/bank`; a 15% reserve requires at most 40,137.
The separate fill front must close that gap for the lofty goal.

## Internal memory

All sizes below are requested bytes, excluding heap headers and task control
blocks. The S3 context sizes are checked by the emulated S3 build, not inferred
from the host ABI.

| Allocation | Fast QPSK cost |
|---|---:|
| Existing `shaper_store` | 65,536 B, unchanged |
| Four-sample contents, worst payload (9/10) | table 4,096 + 3 frames 50,016 + dummy 1,696 + payload 7,264 = 63,072 B |
| Eight-sample contents, worst payload | table 8,192 + 2 frames 33,344 + dummy 1,696 + payload 7,264 = 50,496 B |
| Ring/payload/dummy heap | **0 B** |
| Fast context | 16,816 B; 17,488 B for 4/5 and 8/9; 17,680 B for 9/10 |
| Canonical workspace moved from PSRAM | 12,420 B, of which 4,320 B is reference parity workspace |
| Resident byte BCH table | 8,192 B for MODCODs 4–9; 0 B for 10/11 |
| Total encoder heap | 37,428 B; 38,100 B for 4/5; 29,908/30,100 B for 8/9 and 9/10 |
| Existing encoder task stack | 4,096 B, unchanged |
| Existing `dvb` / `enc` statics | 13,504 / 440 B, unchanged |

The DAC still reserves 131,072 B, including its unused final words. The radio
worker still requests a 6,144 B stack. Taps (1,440 B) and USB dump buffering
remain in PSRAM. These shared baseline costs are not new allocations.

Compared with a clean 3c4f815 build, linked IRAM text grows 2,600 B and end
padding shrinks 40 B: **2,560 B more internal SRAM reserved**, with no static
data growth. `.dram0.dummy` reflects that same IRAM alias; do not count it twice.
Flash text grows 124 B and read-only data 992 B, including string/link alignment.

Relative to the brief's approximate 53 KB QPSK Wi-Fi-up free heap, the workspace,
BCH and linked-code deltas predict about 29–38 KB free, depending on MODCOD.
Unchanged 8PSK modes lose the 2,560 B linked-code reservation; a former 21 KB
minimum would become about 18.4 KB. Neither prediction establishes Wi-Fi startup
success. Check `free_internal`, `largest_internal`, `min_free_internal`, the
requested/allocated heap fields, and all `layout` addresses on the board.

Keeping four phases in two bits per symbol would save another 25,008 B across
three slots, but needs a different ring/unpack contract. It is unnecessary for
the current fit. A full-frame raw mapping temporary would add 33,282 B; it was
rejected. The canonical workspace is retained so every encoder stage stays
individually comparable with `host/dvbs2.py`.

## Offline verification

[The offline records](../results/2026-10-17-qpsk-encoder/README.md) contain the
model, linked-size comparison, stream CRCs and LeanDVB results. Every new QPSK
configuration recovered 38 contiguous bit-exact frames with zero LDPC flips,
BCH corrections/failures or payload bit errors. The full stream test checks
every DAC word against the plain encoder, repeated commands, allocation failures
and slot layouts. All five published CRCs remain unchanged.

```
python3 tests/test_dvbs2_c.py
python3 tests/test_encoder_tables.py
python3 tests/test_tx_command.py
python3 tests/test_stream.py notes/qpsk-stream --receiver /path/to/leandvb
```

The encoder QEMU app passes all 140 FEC CRCs, 14 PIE stage checks and 448 packed
frame/guard cases. The shaper QEMU app passes 324 existing table/kernel checks,
3,360 sample-glue cases and 39,170 pack/unpack cases, including QPSK radix-four
kernels and all three unpack formats. See `tests/encoder_pie/README.md` and
`tests/shaper_pie/README.md` for exact emulator commands. IDF v6.1 builds pass.

## Coordinator commands, radio off

Write results to a fresh directory. The coordinator builds/flashes using its
normal workflow. Each command below is a separate run of about a minute or less.
The CRCs below assume the existing stored callsign is `N0CALL`; read `info`
first, and use the actual callsign for payload checks. Do not change a station's
callsign just to reproduce a CRC.

Before running: require `ESP_OK`, successful allocations and zero encoder
failures throughout. `fecbench` must retain all 140 CRCs and 14 zero-mismatch
PIE checks; those checks now include packed QPSK. `membench` must retain every
existing sample checksum. A dump must have matching overlap and recover at
least 30 contiguous bit-exact frames with no corrections or failures. A paced
run must have zero underruns, late fills and fatal deadlines, and positive
minimum slack. For QPSK `20m`, mean and live maximum encoding must be at most
339,476 cycles/frame. The stretch encoder criterion is 254,607 cycles/frame.
Whole-bank fill must be below 47,220 cycles; reserve 15% only if it is at most
40,137 cycles with minimum slack at least 7,083. Report failure of the stricter
fill criterion honestly; the existing nine-symbol pulse is not predicted to
meet it. Unchanged 8PSK timing regressions above 5% require investigation.

```sh
PY=python3
OUT=results/2026-10-17-qpsk-board
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
CALL=N0CALL  # use the callsign reported by info
mkdir "$OUT"
"$PY" host/board.py info --reset --out "$OUT/runs.jsonl"
"$PY" host/board.py membench --timeout 60 --out "$OUT/runs.jsonl"
"$PY" host/board.py 'fecbench pie' --timeout 60 --out "$OUT/fec.jsonl"
"$PY" tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only
"$PY" host/board.py 'wifimem 8 10m' --timeout 60 --out "$OUT/heap.jsonl"
"$PY" host/board.py 'wifimem 11 5m' --timeout 60 --out "$OUT/heap.jsonl"
for rate in 5m 10m 10m80 20m; do
    for mc in 4 11; do
        "$PY" host/board.py "dvbpace $mc $rate 30" --timeout 60 --out "$OUT/pace.jsonl"
        "$PY" host/board.py "dvbdump $mc $rate 40" --timeout 60 \
            --save "$OUT/$mc-$rate.iq" --out "$OUT/dumps.jsonl"
    done
done
```

Both Wi-Fi probes leave RF off: NULL mode, no keying or DAC grant. Require
successful startup/deinit, internal workspace/context/BCH addresses and
`ring_heap_bytes:0`. Require at least 24,000 B free after Wi-Fi startup in these
QPSK modes; lower values challenge the memory estimate. MODCOD 8 is the largest
encoder heap. MODCOD 11 has the largest payload/store occupancy.

Default dump decimation is two at eight samples/symbol and one at four, so each
40-frame dump is 21,300,480 B. Decode with:

```sh
for mc in 4 11; do
    for spec in '5m 20000000 5000000' '10m 40000000 10000000' \
                '10m80 40000000 10000000' '20m 80000000 20000000'; do
        set -- $spec
        "$PY" host/onair.py "$OUT/$mc-$1.iq" --rate "$2" --symbol-rate "$3" \
            --scale 512 --modcod "$mc" --stream 40 --callsign "$CALL" --receiver "$RX"
    done
done
```

This decode loop uses a POSIX shell/Bash (`set -- $spec` splits the fields).
Host-predicted default dump CRCs, with `N0CALL`:

| MODCOD | `5m` / `10m80` | `10m` / `20m` |
|---|---:|---:|
| 4 | 3933043456 | 2594131925 |
| 11 | 1552954365 | 2779934507 |

Then repeat unchanged configurations (all counts/levels/decimations explicit):

```sh
"$PY" host/board.py 'dvbdump 4 40' --timeout 60 --save "$OUT/old-4.iq" --out "$OUT/regression.jsonl"
for mc in 13 15; do
    for rate in 5m 10m; do
        "$PY" host/board.py "dvbdump $mc $rate 40 128 1" --timeout 60 \
            --save "$OUT/old-$mc-$rate.iq" --out "$OUT/regression.jsonl"
    done
    "$PY" host/board.py "dvbpace $mc 20m 30" --timeout 60 --out "$OUT/pace.jsonl"
done
```

Require CRCs 1435641601 (old QPSK), 1937985440/1163322913 (13/15 at 5m), and
474263962/1757765864 (13/15 at 10m). Before adoption,
extend the new QPSK dump/pace matrix to MODCODs 5–10, particularly 8 and 9.
Host loopback covers them; silicon has not yet done so.
