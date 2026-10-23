# Radio-off self-check

Pass means the firmware revision matches the checkout, all seven fixed-mode dumps match their
original CRCs, the VCM dump matches its host model, all decode as contiguous exact
payloads in LeanDVB, every paced run keeps
its bank deadlines, the queued modes keep their internal RAM floor, and nothing calls the heap
after boot. A failure makes
the final exit status nonzero. The script prints the complete criteria before any command,
keeps going after a failed dump/decode/pace, and writes each verdict to `summary.tsv`.

Nothing transmits. The only commands sent are `info`, `vcm`, `dvbdump` and `dvbpace`.
The script resets once, takes exclusive board ownership, never flashes, and never changes
the stored callsign. No samples play through the DAC. No SDR is needed.

## Prepare

Use Python 3.12 or later with numpy, scipy and pyserial in the [separate host shell](../../README.md#quick-start).
Build and flash the committed
ESP-IDF v6.1 firmware separately. The contained-test callsign **N0CALL must already be stored**;
the example refuses that criterion if a different callsign is present. Never use N0CALL
as an assigned callsign for a radiating transmission. A reset selects synthetic payload
input so the test does not depend on a SPI sender.

```sh
python3 -m pip install numpy scipy pyserial
python3 examples/self-check/check.py --plan
```

LeanDVB is an independent, separately licensed program. The builder clones the
[pinned LeanSDR work revision](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3)
under ignored `notes/`, applies small portability edits and extends the acquisition
search to the longest DVB-S2 frame, then builds just the receiver. It needs Git, Make
and a C++ compiler (Apple Command Line Tools on macOS; `build-essential` on Debian/Ubuntu).
It neither copies receiver source into the public tree nor changes its decoding algorithms.
An existing receiver directory must match the pinned revision; use a new directory if unsure.

```sh
python3 examples/self-check/build_receiver.py
```

The helper uses stock `-d -d` LDPC/BCH diagnostics. It also reads the old instrumented
receiver's extra syndrome counts when present. Missing syndrome counts stay `null`,
never zero; the normal gate requires zero decoder flips, zero BCH corrections/failures
and an exact payload. [LeanDVB manual](https://www.pabr.org/radio/leandvb/leandvb.en.html).

`host/decode.py` and the [live receiver](../live-video/README.md#uhd-or-saved-iq)
also accept `--ldpc-helper PATH` with a compatible, separately installed receiver/helper
pair. This mode requires one known `--modcod` and normal frames, uses one helper,
and refuses `--pattern`. The stock builder above does not provide that pair.
Soft reports retain BCH results and leave unavailable LDPC flip/parity counts `null`.
They require balanced `S2HELPER` EOF diagnostics and per-attempt `S2SOFT` records.
The clean self-check above keeps its original hard-decoder requirements.

For a synthetic 16-frame dump, with rates matching the dump:

```sh
python3 host/decode.py dump.iq --rate 80e6 --symbol-rate 20e6 \
  --modcod 13 --stream 16 --receiver "$SOFT_RX" --ldpc-helper "$LDPC_HELPER"
```

## Run

After you flash the committed revision, close other serial clients and run:

```sh
python3 examples/self-check/check.py notes/public-check \
  --receiver "$PWD/notes/leansdr/src/apps/leandvb"
```

Use a fresh output directory on every run. `--port` overrides `/dev/cu.usbmodem101`;
the board helper also detects one unambiguous native USB port when that default is missing.
`LEANDVB` can supply the receiver path instead of `--receiver`.

The expected duration is about five minutes, depending on USB and receiver speed.
There are seven 16-frame fixed-mode dumps, one 32-frame VCM dump,
thirteen paces (nine 5 s and four 10 s).
Each pace adds the stored callsign's start/end IDs to its payload time. N0CALL adds
10.44 s per pace, so pacing alone accounts for about 221 s. Dump transport and decoding
make up the rest. A failing or stalled device can take longer because each command times out.

The VCM dump cycles 4/11/13/17 every frame at 5 Msym/s using automatic axes.
It requires matching transport CRCs and sample geometry, at least 30 contiguous
exact host-model BBFRAMEs covering every mode, and zero FEC corrections. A 5 s VCM pace also enforces deadlines
and at least 20 KB minimum free internal RAM. VCM is turned off again even if the dump fails. [Scope and OTA reruns](../../docs/vcm.md).

The selected modes cover 1 Msym/s QPSK, 5 Msym/s 8PSK, 20 Msym/s QPSK/8PSK,
26.67 Msym/s QPSK/8PSK and 33.33 Msym/s QPSK. The new 8PSK dump uses MODCOD 17;
10 s paces cover 13, 15 and 17 at `26m667`. Paces also cover QPSK at 5/10 Msym/s
and 8PSK at 10 Msym/s. CI checks every supported modulation/rate combination on the host.

N is the number of complete data frames in the pinned dump layout, excluding
standard dummy PLFRAMEs and any final partial frame. LeanDVB must return at least
N-2 contiguous QPSK frames. 8PSK permits at most one leading acquisition frame
and three trailing data frames lost at EOF. The 17/3/80/16 dump spans 16 nominal
frame lengths but contains 12 complete data frames, 22 dummies and part of the
next data frame. Its 11 decoded payload frames meet the existing EOF allowance.
`decode.py --dummy-before` supplies this known schedule; it counts nonempty
decoded BBFRAMEs separately from nominal duration and expected complete data frames.
There is no tolerance for payload errors, LDPC flips or BCH corrections. The decoder adds
4,096 zero samples only to receiver input for pipeline lookahead, outside the dump CRC.
CRC fixtures use peak 128, pilots on and callsign N0CALL. The [frame layouts and
goldens](../../tests/data/README.md) are checked against the independent host model.

All paces require ESP_OK, zero underruns/late fills/late switches/fatal lateness, positive
slack, a live encoder with all four PIE stages, at least 512 B encoder stack headroom,
and `shaped_words + reused_words == fill_words`. The 36 MHz 8PSK and 40 MHz QPSK queues must not starve and
must keep at least 20,000 B minimum free internal RAM.

8PSK `26m667` additionally checks the keyed eee5153 figures with explicit profiling
allowances of 4,500 fill cycles and 10,000 encoder cycles:

| MODCOD | Max fill, cycles | Encoder mean / live max, cycles | Max steady dummies/s |
|---|---:|---:|---:|
| 13 | 43,919 | 182,076 / 249,463 | 180 |
| 15 | 43,303 | 186,477 / 261,019 | 220 |
| 17 | 43,463 | 168,719 / 223,647 | 6 |

The measured 20 s paces added up to 3,894 fill and 7,023 mean encoder cycles,
with 173.6 / 213.1 / 5.0 steady dummies per second. These limits bound that cost;
they do not assert zero dummies or full-rate payload in a profiled pace. At most
four startup dummies and one cold production-bound miss are allowed. All 54 pages
must be admitted automatically. For payload duration T and steady-dummy cap C,
at least `floor((T*80e6/3 - (C+4)*3330)/22194)-1` data frames must play. The extra
frame accounts for the ending partial frame. This rejects excessive lost airtime
independently of the dummy counters. [Keyed and pace evidence](../../docs/measurements.md#8psk-at-36-mhz).

Other modes enforce functional deadlines. These checks do not enforce a 15% reserve.
The 40 MHz mode met 15% reserve on both cores in one 30 s keyed run
([e96795b](../../docs/measurements.md#40-mhz)). Compare saved timing and heap records
when evaluating a performance change.

The output contains the host commit and expected firmware revision, the board's actual
`info` record, raw command JSON lines, IQ dumps, receiver logs/JSON and a pass/fail summary.
Preserve a completed directory before changing firmware. The example is the ongoing
board regression check.

## Decode one dump

```sh
python3 host/board.py "dvbdump 11 33m333 16 128 1" --save qpsk.iq
python3 host/decode.py qpsk.iq --rate 80000000 --symbol-rate 33333333.333333332 \
  --rolloff 0.20 --modcod 11 --stream 16 --callsign N0CALL \
  --receiver "$PWD/notes/leansdr/src/apps/leandvb" --log receiver.log
```

Use the actual stored callsign when decoding other dumps. This is digital interoperability;
RF sample continuity, analog bandwidth, emission quality and on-air identification require
a separate [contained RF measurement](../../docs/transmitting.md).
