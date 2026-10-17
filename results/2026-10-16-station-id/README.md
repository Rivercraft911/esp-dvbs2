# Station ID, radio off

NVS callsign setup, fail-closed RF entry, shaped CW, per-frame GCS headers and a capture decoder
are implemented. **No board, serial port, E200 or RF was used.** `runs.jsonl` records host tests,
QEMU hashes, numerical spectra, build evidence and source hashes from base `058387a` plus this
branch's changes. Long derivations and private model tools stay in ignored `notes/station-id.md`.

All 19 host test scripts pass, including compiled console/RF guards, NVS fault stubs, actual
C Morse decoding, sample-overlap seeks across nine-minute boundaries, duration bounds and C/host
payload-header agreement. The bench test passes with a callsign-bearing synthetic DVB signal
and executable device/FEC fakes. New tests are in CI. ESP-IDF v6.1 builds without warnings;
`IDF_COMPONENT_MANAGER=0` bypasses a sandbox-denied process inspection, with all components local.

**Simulated:** eight 16,380-word QEMU fills match portable C hashes, including the PIE copy,
rise/fall ramps, phase offset and periodic seams. **Modelled:** the largest of those fills is
32,917 pipeline cycles against the existing 85,608-cycle bank period. This is neither an
exhaustive worst case nor silicon timing. The earlier encoder model was 19–22% optimistic on
silicon ([FEC results](../2026-10-16-fec-limit/README.md)); that is not a calibration for CW.
The ID context occupies 1,848 bytes on the S3 ABI. Additional NVS/USB/Wi-Fi heap needs remain a risk.

**Numerical spectrum:** `python3 tests/test_station_id.py --spectrum` compiles the actual C
DAC-word generator. One 60 ms dot with 5 ms raised-cosine edges, held every 10 µs, at nominal
40 MS/s uses 3,200,000 samples including 10 ms zero padding per side, FFT length 4,194,304.
Energy ratio is 10 log10(sum |FFT|² in selected bins / sum |FFT|² in all bins). The 1..50 kHz
offset interval integrates −60.52 dBc, versus −27.81 dBc with hard keying. All bins outside
±1 kHz integrate −49.88 dBc, including quantization harmonics. No RF spectral claim follows.
CW timing/characters and the fetched FCC rule are cited in [hardware.md](../../docs/hardware.md#station-identification).

RF seconds include both IDs and stay within 60 s. Too-short runs refuse before worker creation.
`dvbpace` retains payload seconds and adds IDs for long radio-off checks. ID counters are planned
segments, not proof of delivery. Faults stop immediately and may omit closing ID. Periodic CW
interrupts DVB receiver lock. Startup transients, leakage in spaces, data/CW transitions,
Wi-Fi allocation and DAC contention remain unverified on hardware.

## Coordinator checks, radio off

Only the coordinator runs these. Use the framework Python for host analysis and board.py.
After activating IDF, build/flash this branch using the usual coordinator workflow. These
checks require no SDR. Stop on any failed step. `N0CALL` below is synthetic, never authorization
for RF; clear it after testing and replace it with the assigned call on Monday.

First erase and confirm the empty setting before testing the two refusal paths:

```sh
python3 host/board.py "callsign clear"
python3 host/board.py callsign --reset
python3 host/board.py info
python3 host/board.py "tx 2420 tone128 1"
python3 host/board.py "dvb 2420 4 1"
```

Pass: both reads show `callsign:""`; only then run the last two commands. Each must exit 1
with `no callsign set`, before any radio setup. Do not continue if clearing/read-back failed.
Radio-off commands below also remain available with no stored callsign:

```sh
python3 host/board.py "iddump N0CALL" --timeout 120 --save id.iq
python3 host/morse.py id.iq --rate 10000 --tone 0 --expect N0CALL
python3 host/board.py callsign
```

Pass: host CRC equals dump CRC, `status:ESP_OK`, `overlap_mismatches:0`, 208,800 bytes,
`dac_samples:208800000`, decoder `text:N0CALL` and `match:true`. Stored callsign stays empty.
The 4000:1 dump aliases the tone to DC deliberately; it cannot qualify RF spectrum.

Check persistence, data headers and the integrated timed wrapper without RF:

```sh
python3 host/board.py "callsign n0call"
python3 host/board.py callsign --reset
python3 host/board.py info
python3 host/board.py "dvbdump 4 40" --timeout 120 --save data.iq
python3 host/onair.py data.iq --rate 4000000 --scale 512 --stream 40 --callsign N0CALL \
  --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 host/board.py "dvbpace 4 60" --timeout 120
python3 host/board.py "dvbpace 4 600" --timeout 720
python3 host/board.py "callsign clear"
python3 host/board.py callsign --reset
```

Pass: uppercase `N0CALL` survives reset and appears in `info`; dump CRC matches, zero overlap
mismatches, independent receiver frame lock and complete contiguous payload match including
headers, no FEC errors. Both paced runs require `ESP_OK`, `encoder_failed:false`, zero
`late_fills` and `fatal_late`, positive `min_fill_slack_cycles`, and no normal-encoder underruns.
Expected `id_count` is 2 then 3; `id_samples` is 208,800,000. Planned total samples are
2,817,600,000 (70.44 s) then 24,626,400,000 (615.66 s). Final read must be empty after reset.
These pass criteria establish radio-off behavior only. No configured `tx` or `dvb` is part
of this handoff. Actual RF ID and spectral checks need a separately authorized cabled run.
