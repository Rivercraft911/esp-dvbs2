# Bench setup

Radio-off stream cycle/cache diagnostics, the internal 8PSK frame layout and the Wi-Fi
memory probe are described in [stream profiling](stream-profile.md). Silicon timing must be
measured with `dvbpace`; host and QEMU checks establish correctness and placement only.

## Board

ESP32-S3 rev 0.2 with 8 MB embedded octal PSRAM and 16 MB flash, two 240 MHz cores (read by
`python3 host/board.py info` and esptool). Any S3 with 8 MB PSRAM and an antenna connector
should work. The firmware talks over the chip's own USB port (USB Serial/JTAG).

The board has two USB-C ports:

| Port | Chip | Used for |
|---|---|---|
| Native USB (`USB JTAG_serial debug unit`, 303a:1001) | S3 USB Serial/JTAG | commands, results, flashing, payload tests |
| UART (`USB Single Serial`, 1a86:55d3) | WCH CH343 bridge on UART0, with the usual EN/GPIO0 auto-reset circuit | hardware resets |

## Resets

On macOS an RTS reset through the native port left GPIO0 low, so the chip restarted into its
bootloader (strap register 0x23, force-download flag 0). `host/board.py --reset` therefore pulls
EN low through the CH343 port when it is plugged in, and otherwise falls back to esptool's
watchdog reset. It opens the native port without toggling the reset lines. Over the CH343 port,
esptool's RAM stub upload failed with a checksum error at the default settings; `--no-stub`
works. `idf.py flash` on the native port is unaffected.

## Receiving the signal

- Take RF from the board's U.FL connector with a U.FL-to-SMA pigtail.
- Put at least 30 dB of attenuation between the board and the receiver. The radio can put out
  around +20 dBm; esp-sdr-zephyr's S3-to-S3 tests used 30 dB.
- An E200 SDR records the signal for offline analysis and decoding. It reports an AD9364
  (iio context `hw_model: Analog Devices PlutoSDR Rev.C (Z7010-AD9364)`, `ad9361-phy,model: ad9364`,
  firmware v0.34), which supports up to 61.44 MS/s and 56 MHz of RF bandwidth.

## Station identification

[47 CFR §97.119(a)](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-B/section-97.119)
requires the assigned callsign on the transmitting channel “at the end of each communication,
and at least every 10 minutes during a communication”. Paragraph (b)(1) permits CW and caps
automatic ID at 20 WPM; (c) covers slash-separated indicators. Fetched 2026-10-16.
The operator supplies an authorized assignment and required indicators. Syntax cannot establish that.

`callsign <CALL>` accepts 3..23 ASCII characters with a letter and digit, allows single internal
slashes, normalizes uppercase and commits to nonvolatile storage (NVS). `callsign` reads it,
`callsign clear` erases it, and `info` reports it. Invalid syntax preserves the old value;
storage faults disable RF without automatically erasing NVS. With no callsign, `tx` and `dvb`
refuse before waveform/encoder setup, Wi-Fi startup or calibration. The public radio entry also
refuses, and so does `wifimem`, which starts Wi-Fi without keying. Radio-off `dvbdump`,
`dvbpace`, `membench` and `fecbench` still work.

CW runs before/after each successful keyed run and repeats every 540 s of the nominal sample
timeline. Requested RF seconds include both IDs and stay limited to 60 s; too-short runs fail
before worker creation. Synthetic `N0CALL` takes 5.22 s per ID, leaving 19.56 s of data in a
30 s run. With a callsign configured, `dvbpace` exercises the same wrapper, adding IDs to its
requested **payload** duration so a 600 s test covers periodic ID. The payload clock pauses
during CW; the bank schedule and DAC epoch continue. A fill/deadline fault stops immediately
and cannot guarantee closing ID. Periodic CW requires DVB receiver reacquisition.

The chosen +1 MHz tone, peak 128 DAC counts, avoids nominal DC leakage; include its offset
when choosing the licensed channel. At 80 MS/s (`dvb ... 10m80|20m`) every ID duration doubles in
samples, so the timing stays 20 WPM, and the 40-sample tone row plays at +2 MHz. [ITU-R M.1677-1, Annex 1, Part I §§1–2, pp. 2–3](https://www.itu.int/dms_pubrec/itu-r/rec/m/r-rec-m.1677-1-200910-i!!pdf-e.pdf)
supplies Morse characters/timing. PARIS has 50 dot units, giving 60 s/(20 × 50) = 60 ms dots.
Edges use (1 − cos(πt/5 ms))/2, held every 10 µs and rounded to one DAC count. The **host FFT
estimate** integrates −60.5 dBc from 1 to 50 kHz offset for one dot, versus −27.8 dBc for hard
keying. See [ID host results](../results/2026-10-16-station-id/README.md) for the estimate and
cycle model. Neither qualifies RF: startup transients, carrier leakage in spaces, data/CW
boundary clicks, spectrum and deadlines with DAC/Wi-Fi traffic still need cabled measurements.

Every nonempty GCS frame starts with 40 bytes: `ESP-DVBS2 ID=CALL`, spaces through byte 38,
then LF at byte 39. Receivers read it at the start of each decoded BBFRAME data field.
Seeded bytes keep their original positions; empty underrun frames stay empty. Verify with
`host/onair.py ... --stream N --callsign CALL`. Omitting the callsign keeps legacy checks.
The header is supplemental; CW is the intended regulatory ID. `host/bench.py` reads `info`,
waits the modelled opening CW plus `--settle`, and forwards the callsign to analysis.

`iddump [CALL]` never writes NVS or starts the radio. It dumps actual bank samples at 10 kS/s,
intentionally aliasing +1 MHz to DC for envelope checks. Use `host/morse.py id.iq --rate 10000
--tone 0 --expect CALL`. For isolated CW capture windows, use `--start`, `--seconds`, the actual
sample rate, and `--tone` at +1 MHz plus carrier error, within about 100 Hz. Formats are `i16`
and `f32`. See [coordinator commands and pass criteria](../results/2026-10-16-station-id/README.md#coordinator-checks-radio-off).

## Radio experiment

Only the coordinator flashes or operates the board for `exp/radio`. Connect the attenuation
before sending a valid `tx` command: Wi-Fi PHY calibration precedes the timed DAC playback.
The command is `tx <integer MHz> <tone|two|tag> <1..60 seconds> [gain]`, with centers restricted
to 2390–2450 MHz and vendor gain steps 0–17, default 0. Gain is not calibrated in dBm.
`tone` is +1 MHz relative to the requested center; `two` is +1 and +3 MHz. Keep the actual
signal within the licensed band when choosing the center.

Analyze active capture interiors with `python3 host/capture.py file.iq --format i16
--rate 10000000 --signal tone` (`f32` also works). Power is dBFS, not dBm. The `tag` phase
sequence checks individual 40 MS/s samples and needs at least 50 MS/s complex capture and a
qualified 40 MHz RF passband. The E200's AD9364 is specified for [up to 56 MHz channel
bandwidth](https://www.analog.com/en/products/ad9364.html), but its passband has not been measured
here, so it is not yet qualified for this full-rate continuity test. Rejected tag acquisition is
inconclusive. No RF result is claimed until the coordinator measures it.

## Frequencies and licence

The first tests use 2390–2450 MHz, in the US 13 cm amateur allocation. Transmitting there needs
an amateur licence, and running the radio outside Wi-Fi channels means the module's Wi-Fi
certification does not cover it. Keep the antenna off and the signal in a cable until a licensed
operator is on the air.

## Monday checklist

1. Give this session exclusive ownership of the board and SDR. Connect the board U.FL to an
   SMA pigtail, then **30–40 dB of attenuators**, then coax to E200 **RX1**. Leave antennas off.
   Record each attenuator in path order and describe the whole cable path. Do not count an
   unmeasured cable loss as attenuation. Explicitly record whether anything radiates.
2. Use the already flashed `dvb` firmware. Its command sends QPSK at 1 Msym/s, with eight
   repeated normal frames, pilots and roll-off 0.35 (`firmware/main/dvbtx.c:3`,
   `firmware/main/dvbtx.c:9`, `firmware/main/dvbtx.c:31`, `firmware/main/dvbtx.c:32`).
   Start with MODCOD 4, peak 128 and board gain 0. Peak 128 is the initial setting suggested
   by the recorded amplitude sweep, not a calibrated RF power
   ([radio results, Drive level](../results/2026-10-16-radio/README.md#drive-level)).
3. Set E200 LO 2420 MHz, sample rate 4 MS/s, bandwidth 2 MHz, manual gain 0 dB, port
   `A_BALANCED`. These are starting settings for the cabled test. The ADI driver documents
   RX1 and the separate RX/TX LOs under
   [General attribute naming convention](https://analogdevicesinc.github.io/linux/drivers/iio-transceiver/ad9361/#general-attribute-naming-convention).
   `host/sdr.py` powers down `altvoltage1`, verifies all eight requested attributes, and
   requires scan channels I/Q at indices 0/1 with `le:S12/16>>0`. Any mismatch stops the run.
4. From the checkout being tested, first print the plan. Replace **20 10** and the cable
   description with the actual installed path. Use the framework Python if IDF is activated:

   ```sh
   /Library/Frameworks/Python.framework/Versions/3.13/bin/python3 host/bench.py dvb \
     --topic monday-cable --attenuation 20 10 \
     --cable 'U.FL-SMA pigtail -> 20 dB -> 10 dB -> SMA coax -> E200 RX1; no antennas' \
     --radiates no --dry-run \
     --libiio /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/libiio \
     --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
   ```

   After checking the path and frequency under your amateur licence, run the **same command**
   with `--dry-run` replaced by `--authorize-tx`. Omitting the RF-path options prompts on a
   terminal; a noninteractive run refuses missing fields. The authorization applies to that
   one timed run. Neither command resets or flashes the board.
5. Defaults send `dvb 2420 4 30 0 128`, wait the configured opening CW duration plus 2 s after starting `board.py`, then request
   **one 4,000,000-sample buffer**, 1 s and 16,000,000 bytes of int16 I/Q. The generated
   capture command uses `iio_readdev -b 4000000 -s 4000000` (libiio v0.25
   [buffer creation and refill](https://github.com/analogdevicesinc/libiio/blob/v0.25/tests/iio_readdev.c#L398)).
   Settings are read back again immediately before acquisition. The firmware has no RF-start
   acknowledgement; the 2 s settle is an assumption to check on Monday. The tool refuses
   capture if the board has already ended or the planned TX time remaining is too short.
6. Check the final JSON status and both `results/<local-date>-monday-cable/runs.jsonl` and
   `captures.jsonl`. They retain the RF path, board `info` and timed-command records,
   host revision/tool hashes, read-backs, libiio versions, capture times/hash, and analysis.
   Keep the local `.iq`, `.iq.json`, board log and receiver log with the run. A successful
   DVB run requires known-symbol MER recovery, LeanDVB frame lock and a complete eight-frame
   payload match, plus a successful board result without late fills or fatal deadlines.
   MER is modulation error ratio, the fitted reference-symbol power divided by residual
   error power. Carrier offset and a constant clock error are removed; phase noise and
   channel distortion remain. This estimator is qualified with synthetic signals and saved
   radio-off dumps, not RF captures. A single-buffer file length does not prove RF continuity.
   On errors, the tool waits for the finite board command and records its outcome. Ctrl-C
   cannot directly stop firmware RF; keep the cable connected until the timed run has ended.

For an initial tone instead, use `tx` with the same RF-path and libiio options and add
`--bandwidth 4000000`. The default `tone128` and gain 0 command lasts 30 s including its start/end IDs; `host/capture.py`
reports tone/image/leak and envelope/phase statistics with full scale 2048, in dBFS, not dBm.
For offline reanalysis (no devices are opened):

```sh
python3 host/onair.py results/<date>-monday-cable/<run-id>.iq \
  --rate 4000000 --scale 2048 --auto-shift --mer --callsign YOURCALL \
  --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 host/onair.py capture.iq --rate 4000000 --scale 2048 --auto-shift --mer-only --callsign YOURCALL
```

## 8PSK radio-off checks

`dvb`, `dvbdump` and `dvbpace` accept MODCOD 12..17 with `5m` or `10m` immediately
before the frame count/duration. QPSK 4..11 keeps its existing syntax. For example,
`dvbdump 13 5m 40 128 1` and `dvbpace 15 10m 60`. The last dump argument before an
optional stall is decimation: it must divide sps and leave at least two samples/symbol.
Defaults are 2 at 5 Msym/s and 1 at 10 Msym/s. Roll-off is 0.35, matching the encoder's
BBHEADER (`firmware/components/dvbs2/include/dvbs2.h`, format comment). The nominal
occupied bandwidth is `(1 + 0.35) * Rs`: 6.75 MHz and 13.5 MHz (EN 302 307-1 5.6).
The rate token goes in the same place in `dvb <MHz> <MODCOD> <5m|10m> <seconds> [gain] [peak]`.
With a callsign stored, every data frame starts with the 40-byte ID header, so add
`--callsign CALL` to `host/onair.py`, and `dvbpace` adds both CW IDs to its payload seconds.

The coordinator runs these after building and flashing the branch under test. These commands
use USB and timed RAM fills, with the radio off. No SDR is needed. In bash:

```sh
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
OUT=notes/8psk-board
mkdir -p "$OUT"
"$PY" host/board.py info --reset --out "$OUT/runs.jsonl"
for mc in 13 15; do
  for rate in 5m 10m; do
    rs=5000000
    if [ "$rate" = 10m ]; then rs=10000000; fi
    "$PY" host/board.py "dvbdump $mc $rate 40 128 1" --timeout 180 \
      --save "$OUT/$mc-$rate.iq" --out "$OUT/runs.jsonl"
    "$PY" host/onair.py "$OUT/$mc-$rate.iq" --rate 40000000 --symbol-rate "$rs" \
      --scale 512 --modcod "$mc" --stream 40 --receiver "$RX" --log "$OUT/$mc-$rate-rx.log"
    "$PY" host/board.py "dvbpace $mc $rate 60" --timeout 120 --out "$OUT/runs.jsonl"
    "$PY" host/board.py "dvbdump $mc $rate 3 128 1" --timeout 60 \
      --save "$OUT/$mc-$rate-mer.iq" --out "$OUT/runs.jsonl"
    "$PY" host/onair.py "$OUT/$mc-$rate-mer.iq" --rate 40000000 --symbol-rate "$rs" \
      --scale 512 --modcod "$mc" --stream 3 --mer-only --shift 0
  done
done
```

`wifimem 13 5m` (or any mode) measures the internal heap around Wi-Fi start-up next to that
mode's memory without keying; see [stream profiling](stream-profile.md#wi-fi-start-up-memory).

Pass: every board status is `ESP_OK`, `encoder_failed=false`, dump CRC matches the host,
`overlap_mismatches=0`; LeanDVB reports `payload_match=true`, at least 30 payload frames,
`bch_failed_frames=0`, `ldpc_checks_before=0` and `status_complete=true`. Each 60 s pace
must have `late_fills=0`, `fatal_late=0`, positive `min_fill_slack_cycles` and no underruns.
Compare the whole-bank `max_fill_cycles` with 85,608 cycles, not the single-call maximum.
Full-rate MER must acquire and exceed 38 dB. This is a model-based acceptance floor, not
an RF specification. The taps pre-correct the DAC hold, which a dump does not have, so
the host table models give 43.8 dB at 5 Msym/s and 40.6 dB at 10 Msym/s at peak 128
(41.6 dB at 10 Msym/s once an ideal hold is applied).

Also rerun `dvbdump 4 40` / `dvbpace 4 60` for QPSK regression. To provoke dummy insertion,
use `dvbpace 13 10m 60 128 10`: require nonzero underruns with zero late fills and no
encoder failure. A stalled dump can use `dvbdump 13 10m 40 128 1 2000` with a 300 s
host timeout; require at least one decoded data frame, bit-exact contiguous payload with
`--stream 40`, no BCH failures and zero overlap mismatches. It need not recover 30 data
frames. Dummies carry no bytes, and LeanDVB skips them. MER on a stalled stream requires
its dummy positions, supplied as `--dummy-before <data-frame-index> ...` (repeat an index
for consecutive dummies). Firmware reports the count only, so use unstalled dumps for MER.

Keep the records, receiver logs and firmware revision in a new `results/<date>-8psk-board/`
run with a README. No RF or sustained 8PSK silicon timing is established by host/QEMU tests.
