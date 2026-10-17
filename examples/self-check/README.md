# Radio-off self-check

Pass means the firmware revision matches the checkout, all six sample dumps match their
original CRCs and decode as contiguous exact payloads in LeanDVB, every paced run keeps
its bank deadlines, and Wi-Fi startup retains its measured heap floor. A failure makes
the final exit status nonzero. The script prints the complete criteria before any command,
keeps going after a failed dump/decode/pace, and writes each verdict to `summary.tsv`.

Nothing transmits. The only commands sent are `info`, `dvbdump`, `dvbpace` and `wifimem`.
The script resets once, takes exclusive board ownership, never flashes, and never changes
the stored callsign. The 33m333 pace and heap check start Wi-Fi in NULL/promiscuous receive
mode; no samples play through the DAC. No SDR is needed.

## Prepare

Use Python 3.12 or later with numpy, scipy and pyserial. Build and flash the committed
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

## Run

After the coordinator has flashed the committed revision and released the board:

```sh
python3 examples/self-check/check.py notes/public-check \
  --receiver "$PWD/notes/leansdr/src/apps/leandvb"
```

Use a fresh output directory on every run. `--port` overrides `/dev/cu.usbmodem101`;
the board helper also detects one unambiguous native USB port when that default is missing.
`LEANDVB` can supply the receiver path instead of `--receiver`.

The expected duration is about five minutes, depending on USB and receiver speed.
There are six 16-frame dumps, nine paces (eight 10 s and one 20 s) and one heap check.
Each pace adds the stored callsign's start/end IDs to its payload time. N0CALL adds
10.44 s per pace, so pacing alone accounts for about 194 s. Dump transport and decoding
make up the rest. A failing or stalled device can take longer because each command times out.

The selected modes cover 1 Msym/s QPSK, 5 Msym/s 8PSK, 20 Msym/s QPSK/8PSK,
26.67 Msym/s QPSK and 33.33 Msym/s QPSK. Paces also cover QPSK at 5/10 Msym/s
and 8PSK at 10 Msym/s. CI checks every supported modulation/rate combination on the host.

For N dumped frames, LeanDVB must return at least N-2 contiguous QPSK frames.
8PSK permits at most one leading acquisition frame and three trailing frames lost at EOF.
There is no tolerance for payload errors, LDPC flips or BCH corrections. The decoder adds
4,096 zero samples only to receiver input for pipeline lookahead, outside the dump CRC.
CRC fixtures use peak 128, pilots on and callsign N0CALL.

All paces require ESP_OK, zero underruns/late fills/late switches/fatal lateness, positive
slack, a live encoder with all four PIE stages, at least 512 B encoder stack headroom,
and `shaped_words + reused_words == fill_words`. The 40 MHz queue must not starve and
must retain at least 20,000 B internal RAM with Wi-Fi up. The separate 8PSK 9/10 `5m`
heap check retains the original 22,351 B Wi-Fi-up and command-minimum floors.

This checks functional deadlines. It does not impose the archived research candidate's
2% cycle ceilings or claim that the 40 MHz mode meets the unachieved 15% reserve target.
Compare saved timing and heap records when evaluating a performance change.

The output contains the host commit and expected firmware revision, the board's actual
`info` record, raw command JSON lines, IQ dumps, receiver logs/JSON and a pass/fail summary.
Preserve a completed directory before changing firmware. The example is the ongoing
board regression check; the old coordinator runner lives only in the archive.

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
