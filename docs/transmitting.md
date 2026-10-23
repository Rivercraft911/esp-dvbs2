# Transmitting and receiving

The transceiver is two apps on two ESP32-S3s. The transmitter keys DVB-S2 for 1 to 540 s per
command, its Morse ID included, and the receiver listens for up to 600 s and checks every frame
on the chip. Both take one command per line over the S3's native USB port and answer in JSON.
This page lists every command in both apps, the boards they ran on, the builds for each, and the
limits on keying the radio.

Flashing starts no transmission. The transmitter waits for a command, and the receiver app has
no transmit command. Key the transmitter only as a licensed amateur operator, on a frequency
your licence covers, into a dummy load, inside a closed shielded box, or through at least 30 dB
of attenuation into a receiver ([licence and containment](#licence-and-containment)). Start with
the radio-off [self-check](../examples/self-check/README.md).

## Board and USB

Both apps are built for an ESP32-S3 with 8 MB of octal PSRAM. The records name Seeed XIAO
ESP32-S3 boards for the transmitter and the receiver alike
([keyed table](../results/2026-10-23-tx-table/README.md),
[first S3 receive](../results/2026-10-22-rx-first/README.md)). The transmitter's `info` on those
boards reports chip revision v0.2, 240 MHz and 8,388,608 bytes of PSRAM
([sweep records](../results/2026-10-23-rx-sweep/runs.jsonl)). The transmitter's default
configuration is for the N16R8 DevKit board the project started on, which has twice the flash.

| Board | Flash | PSRAM | USB-C ports | Transmitter build | Receiver build |
|---|---:|---:|---|---|---|
| XIAO ESP32-S3 | 8 MB | 8 MB octal | Native USB only | `sdkconfig.defaults` + `sdkconfig.xiao` | `sdkconfig.defaults` |
| N16R8 DevKit | 16 MB | 8 MB octal | Native USB and a CH343 UART bridge | `sdkconfig.defaults` | `sdkconfig.defaults` |

The XIAO overlay does two things: it sets 8 MB of flash, and it makes the XIAO pins the default
for [SPI input](spi-input.md#pins). The receiver has one configuration. It declares 8 MB of
flash, which fits both boards, and keeps a 7 MB capture store in PSRAM.

### Building

Use ESP-IDF v6.1 ([Espressif's ESP32-S3 guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html)).
The activation path below is the macOS EIM install used here; use your own export script if IDF
lives elsewhere. The defaults select the `esp32s3` target. The parentheses keep IDF's Python out
of the [host shell](../README.md#quick-start).

```sh
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/rx && idf.py build flash)   # receiver, either board
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/tx && idf.py build flash)   # transmitter, N16R8
```

A XIAO transmitter applies the overlay in a configuration file and build directory of its own,
so an existing `firmware/tx/sdkconfig` stays as it is. `notes/` is ignored by git. Any fresh
paths work; a build directory that was configured for another source tree does not.

```sh
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/tx && \
  idf.py -B ../../notes/build-tx-xiao -D SDKCONFIG="$PWD/../../notes/sdkconfig.tx-xiao" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao' build flash)
```

With two boards plugged in, name the port: `idf.py -p PORT ... build flash`. Leave out `flash`
to build without a board. Both apps stamp the commit into `info`'s `firmware` field, with
`-dirty` for uncommitted changes, when CMake configures the build. A rebuild that does not
reconfigure can keep an old stamp, so run `idf.py reconfigure` before a measurement run and
record the source revision separately as well.

### Ports

| Port | USB ID | Used for |
|---|---|---|
| Native USB (USB Serial/JTAG), `/dev/cu.usbmodem*` on macOS | 303a:1001 | Commands, results and flashing, on both boards |
| CH343 UART bridge, N16R8 DevKit only | 1a86:55d3 | Hardware EN reset by `host/board.py --reset` |

From macOS, an RTS reset through the native port can leave the chip in its bootloader, so
`host/board.py` opens that port without a reset transition (`open_port` explains the line
dance). `--reset` pulls EN low through a CH343 bridge if one is plugged in, and otherwise uses
esptool's watchdog reset over the native port, which needs esptool on the `PATH` or in the
ESP-IDF install. With several boards attached, `--reset` takes the first CH343 bridge it finds,
whichever board that belongs to. One session owns a board: `board.py` and `host/rx.py` lock the
port and refuse to start while another process has it open.

## Console

Every command is one line. The board answers with lines that start with `@` followed by one
JSON object, and ends each command with a record holding `"done":true`. A record with
`"binary_bytes": N` is followed by exactly N raw bytes. A rejected command comes back as a
record with an `error` field, usually the usage line. The transmitter refuses lines longer than
126 characters.

`host/board.py` sends one command, prints each record, and exits 1 if any record has an `error`:

```sh
python3 host/board.py info --reset
python3 host/board.py "dvbdump 15 20m 16 128 1" --save fast.iq
python3 host/board.py "dvbpace 11 26m667 10" --timeout 120 --out notes/pace.jsonl
```

`--save FILE` keeps the binary bytes. `--out FILE` appends the records to a JSON-lines file with
the command and the host time. `--port` picks the board: `/dev/cu.usbmodem101` by default, or
the only `/dev/cu.usbmodem*` if that one is missing. The default `--timeout` is 30 s, extended
while bytes keep arriving. Up to 10 Msym/s the status line below keeps it moving during a run;
give a faster run its length plus a margin.

**Status line.** While `dvb` or `dvbpace` runs at 10 Msym/s or less, the transmitter prints one
`{"cmd":"status",...}` record a second. From the start of the run it counts `frames` played,
`frames_encoded`, `dummies`, `late_fills` and `late_switches`, and with payload input the
`input_bytes` and `input_messages` accepted, `input_rejected` (failed checks or wrong type),
`input_crc_errors`, `input_lost` (sequence gaps) and `input_overruns`. Since the previous line it
gives the encoder's mean and slowest frame and the slowest bank fill, in CPU cycles and as spare
against `frame_period_cycles` and `fill_deadline_cycles`, or null if none ran. With VCM,
`modcod` is the encoder's current one, a frame or more ahead of playback. The command task
prints the line on core 0 while it waits for the run, so neither the encoder nor a fill ever
prints, and a line the USB buffer has no room for is counted in `lines_dropped` instead of waited
for. Each line still costs the encoder one slower frame, up to 47k cycles more. At 5 and
10 Msym/s that leaves 38.8% or more of the frame period spare and the fills unchanged
([gated](../results/2026-10-23-tx-status-gated/README.md)). At 26.67 Msym/s it put dummy frames
into 8PSK 9/10 with TS input ([status timing](../results/2026-10-23-tx-status/README.md)), so
a run above 10 Msym/s prints one status record with `"status_lines":"off above 10 Msym/s"` at
its start and no lines.

`board.py` neither prints nor keeps status records. `--monitor HOST:PORT` sends them and the
command's own records to `host/monitor.py` as UDP datagrams: a 12-byte little-endian header
(`PVMN`, version 1, type 4, a 16-bit sequence number and the low 32 bits of the host's monotonic
clock in microseconds), then the record's JSON. For example,
`python3 host/board.py "dvbpace 14 5m 30 --short" --monitor 127.0.0.1:1235`.

`spiinput`, `spipins`, `vcm` and `rflo` settings last until changed or until the board restarts.
The callsign lives in flash and survives restarts. `--short`, `--fixture` and `--phy-solo`
apply to their own command only. The transmitter's completion record also carries
`main_stack_bytes`, `main_stack_min_free` (the main task's stack size and its fewest unused
bytes since boot) and `heap_allocs_after_boot`.

### Transmitter commands

Radio off. None of these needs a callsign; if one is stored, synthetic payloads carry it.

| Command | Arguments | What it does |
|---|---|---|
| `info` | | Firmware commit, ESP-IDF version, chip revision, CPU clock, PHY status (`radio`), PSRAM, free memory, run-memory use, heap calls since boot, stored callsign |
| `callsign [CALL\|clear]` | CALL as in [station identification](#station-identification) | Reads, stores or erases the callsign |
| `iddump [CALL] [40\|80]` | CALL defaults to the stored one; DAC 40 (default) or 80 MS/s | The Morse ID as the DAC would play it, every 4,000th or 8,000th sample: 10 kS/s int16 I/Q. Writes nothing to flash |
| `dvbdump MODCOD [RATE] FRAMES [PEAK] [DECIM] [STALL] [--short]` | FRAMES 1 to 256; PEAK 16 to 508, default 128; DECIM and STALL below | The samples the DAC would play for FRAMES frames, every DECIM-th one, as int16 I/Q. No Morse ID |
| `dvbpace MODCOD [RATE] SECONDS [PEAK] [STALL] [--short] [--fixture] [--phy-solo]` | SECONDS 1 to 600 of payload | A keyed run's encoder and bank fills against the same deadlines, radio off. With a callsign stored, its IDs come on top of SECONDS |
| `fecbench MODCOD [--short]` | MODCOD 4 to 17; with `--short`, 4 to 10 or 12 to 16 | BCH and LDPC cycles on one frame over 100 runs, with the BCH table in flash and then resident |
| `spiinput`, `spipins`, `spirx`, `spisend` | [Payload input](#payload-input) | Choose the payload source; SPI diagnostics |
| `vcm` | [VCM](#vcm) | Per-frame MODCOD switching for the next runs |
| `rflo direct\|five-sixths [KHZ]` | KHZ -999 to 999, default 0 | [Tuning](#tuning) for keyed runs |

Keyed. Both need a stored callsign and refuse before any radio access without one.

| Command | Arguments | What it does |
|---|---|---|
| `tx MHZ SIGNAL SECONDS [GAIN] [40\|80]` | MHZ 2000 to 2450; SECONDS 1 to 60, IDs included; GAIN 0 to 17, default 0; DAC 40 (default) or 80 MS/s | A [test signal](#keyed-commands) |
| `dvb MHZ MODCOD [RATE] SECONDS [GAIN] [PEAK] [--short] [--fixture] [--phy-solo]` | MHZ 2000 to 2450; SECONDS 1 to 540, IDs included; GAIN 0 to 17, default 0; PEAK 16 to 508, default 128 | DVB-S2 |

A MODCOD names a modulation and code rate. QPSK 4 to 11 are 1/2, 3/5, 2/3, 3/4, 4/5, 5/6, 8/9
and 9/10; 8PSK 12 to 17 are 3/5, 2/3, 3/4, 5/6, 8/9 and 9/10. PEAK sets the drive level in DAC
steps; the 10-bit DAC spans -512 to 511. GAIN is the radio vendor's power index: uncalibrated,
not dBm. `--phy-solo` used to stop Wi-Fi before playback. Neither app starts Wi-Fi any more, so
it is accepted and does nothing.

`dvbdump`'s DECIM defaults to 10 at 1 Msym/s, 2 at `5m` and 1 elsewhere. It must divide the
samples per symbol and be at most half of them, and it must be 1 at `33m333`. The dump's sample
rate is the DAC rate divided by DECIM, and the first record states it. STALL, 0 to 2000 ms and
0 by default, pauses the encoder that long after every frame.

### Rates and frames

| RATE | Symbol rate | DAC | Samples per symbol | Bandwidth | Modulations | `--short` | VCM | `--fixture` |
|---|---:|---:|---:|---:|---|---|---|---|
| omitted | 1 Msym/s | 40 MS/s | 40 | 1.35 MHz | QPSK | QPSK 4 to 10 | QPSK | no |
| `5m` | 5 Msym/s | 40 MS/s | 8 | 6.75 MHz | QPSK, 8PSK | QPSK 4 to 10, 8PSK 12 to 16 | yes, mixed too | no |
| `10m` | 10 Msym/s | 40 MS/s | 4 | 13.5 MHz | QPSK, 8PSK | no | no | no |
| `20m` | 20 Msym/s | 80 MS/s | 4 | 27 MHz | QPSK, 8PSK | no | no | no |
| `26m667` | 80/3 Msym/s | 80 MS/s | 3 | 36 MHz | QPSK, 8PSK | no | no | 8PSK only |
| `33m333` | 100/3 Msym/s | 80 MS/s | 12/5 | 40 MHz | QPSK | no | no | yes |

Bandwidth is the symbol rate times (1 + roll-off): roll-off is 0.35, except 0.20 at `33m333`.
The transmitter has no `1m` token; leave the rate out. Normal frames are 64,800 bits with
pilots. `--short` selects 16,200-bit frames, each carrying (Kbch - 80) / 8 payload bytes in
8,370 QPSK or 5,598 8PSK symbols with pilots. There is no short 9/10. `--fixture` repeats one
prepared synthetic frame, copied in 1,316-byte pieces as real input would be, which times the
encoder without the cost of generating fresh payload. It needs payload input off and normal
frames, and its record says `"prepared_fixture":true`. Pulse spans and kernels per rate are in
[how it works](how-it-works.md#rates).

### Payload input

By default each run sends a synthetic payload: one continuous xorshift32 byte stream (seed 2026)
with the 40-byte station header at the start of every frame. These commands choose another
source for the next runs. The message format, pins and timing are in [SPI input](spi-input.md).

| Command | What it does |
|---|---|
| `spiinput on [ts\|gcs]` | Single-line SPI, up to 20 MHz. TS (MPEG transport stream) by default, or GCS (a plain byte stream) |
| `spiinput quad [ts\|gcs] [replay]` | Quad SPI, up to 40 MHz. `replay` loops eight presealed messages through the real parser with no transfers: a diagnostic, not input |
| `spiinput usb [ts\|gcs]` | The same messages over the USB cable ([protocol](spi-input.md#over-usb-instead)) |
| `spiinput off` | Back to the synthetic payload |
| `spipins devkit\|xiao` or `spipins SCK CS D0 D1 D2 D3 READY` | A pin preset, or seven distinct output-capable GPIOs. Default `devkit`, or `xiao` in the XIAO build. Applies from the next run |
| `spirx MS on\|quad [ts\|gcs]` | Radio off: receive from an external sender for 100 to 120,000 ms and check every message against the test pattern |
| `spisend MS on\|quad [ts\|gcs] [MHZ [MBPS]]` | Test sender for a second S3 wired to the first, no radio: 100 to 120,000 ms; clock 1 to 20 MHz single-line (default 20) or 1 to 40 MHz quad (default 40); offered payload 0 to 140 Mb/s, where 0, the default, is unpaced |

`spiinput` sets up the SPI driver for its wire, and a 4 KB CRC-32 table, when the wire is chosen.
Runs then only start and stop them; check the record for `"status":"ESP_OK"`. `spiinput off`
frees both, and `spiinput usb` frees the driver. `spirx` sets up its own wire for the probe and
gives the selected input its driver back afterwards. Two older spellings still work: `pv` for
`on`, and `ts` or `gcs` alone for quad. The TS replay loops 56 packets
([TS link](../results/2026-10-23-ts-link/README.md)). If a USB run ends without its closing NOP
message within 3 s, the transmitter restarts, which clears every setting above.

### VCM

VCM (variable coding and modulation) changes the MODCOD at frame boundaries
([details and checks](vcm.md)).

| Command | What it does |
|---|---|
| `vcm HOLD MODCOD...` | A pattern of 1 to 16 MODCODs, each held for HOLD frames (1 to 65,535). The run's MODCOD must be the first entry |
| `vcm on` | MODCOD changes arrive in the input stream as control messages, within the starting modulation |
| `vcm mixed` | The same, between QPSK and 8PSK, at `5m` only |
| `vcm off` | One MODCOD per run, the state after boot |

VCM runs at 1 Msym/s (QPSK only) and at `5m`, on normal frames, with synthetic or GCS input and
without `--fixture`. A pattern that mixes QPSK and 8PSK needs `5m`. A run that used VCM says
`"vcm":true`. Check for it, because a restart quietly turns VCM off.

### Tuning

`rflo direct`, the default, runs the PLL at the requested frequency. `rflo five-sixths` asks the
PLL for six-fifths of it and turns on the radio's five-sixths divider; a B200 decoded QPSK 3/4
at 5 Msym/s at 2000 MHz this way ([lower edge](../results/2026-10-20-band-followup/README.md)).
Either takes an optional signed offset of -999 to 999 kHz, and leaving it out resets the offset
to zero. The frequency plus the offset must stay within 2000 to 2450 MHz. Keyed records report
`requested_rf_khz` and `requested_pll_khz`. Those are requests, not measurements; measure the
signal with a receiver.

### Samples over USB

`dvbdump` and `iddump` send little-endian int16 pairs, I then Q, holding the DAC's 10-bit values
(full scale 512). The pinned CRCs in the [self-check](../examples/self-check/README.md) use peak
128 and the callsign N0CALL. `host/decode.py`, `host/morse.py` and
`examples/live-video/receive.py` read these files and accept the same `--format` names:

| Sample type | `--format` | Bytes per complex sample |
|---|---|---:|
| Signed int16, S3 dumps (full scale 512) | `i16`, `s16` | 4 |
| Float32 | `cf32`, `f32` | 8 |

```sh
python3 host/board.py "dvbdump 15 20m 16 128 1" --save fast.iq
python3 host/decode.py fast.iq --rate 80000000 --symbol-rate 20000000 \
  --modcod 15 --stream 16 --callsign N0CALL --receiver /path/to/leandvb
```

### Receiver commands

| Command | Arguments | What it does |
|---|---|---|
| `info` | | App name, firmware commit, MAC address, PSRAM, PHY status (`radio`), whether every mode's run memory fits (`arena_fits`), free memory, arena peaks, heap calls since boot |
| `rxprobe MHZ GAIN SECONDS` | MHZ 2000 to 2450; GAIN 0 to the radio's `gain_max`, which reads 82 on the boards in the records; SECONDS 1 to 600 | Level, DC, clipped samples and the radio's gain field, from every 64th sample |
| `rx MHZ GAIN SECONDS MODCOD [1m\|5m] [ts [pattern] [nonull]]` | As `rxprobe`; MODCOD 4 to 10 or 12 to 16, short frames with pilots; 1 Msym/s by default | Receives live. Streams one binary record per frame, then a JSON summary |
| `dump MHZ GAIN PAIRS` | PAIRS 1 to 3,670,016, which is 229 ms at 16 MS/s | The first PAIRS samples as int8: all I, then all Q, each the 10-bit value divided by 4 and rounded down |
| `upload BYTES` | Even, up to 7,340,032 | Reads a capture of int8 I, Q pairs at 16 MS/s (`.sc8`) into PSRAM and answers with its CRC-32 |
| `replay MODCOD [SKIP] [1m\|5m] [ts [pattern] [nonull]]` | SKIP leaves out every SKIP-th 768 µs unit | Runs the uploaded capture through the same receiver with the radio off, paced as the radio would deliver it |
| `membench MHZ GAIN` | | Cycles to read 4,096 words from a free capture bank while the radio writes another, against plain internal RAM, then with the radio stopped |

Below 2200 MHz the receiver switches to the five-sixths LO by itself. At 5 Msym/s a good frame's
record carries the payload's CRC-32 instead of the payload. A failed frame's record carries its
hard and weak bits. `ts` also deframes every good frame into 188-byte TS packets on the chip and
checks each packet's CRC-8 and continuity counter. `pattern` compares every packet with the
transmitter's test input (the `spiinput ... ts replay` loop, or a sender's counting pattern),
and `nonull` counts null packets (PID 0x1FFF) without passing them on. There is no packet output
yet: the packets are checked and counted. The receiver accepts 8PSK at 1 Msym/s, but the
transmitter sends short 8PSK only at `5m`, so that pairing is untested.

`rx` and `replay` stream binary records that `host/board.py` cannot read, and `upload` needs the
file sent after its `ready` record, so use `host/rx.py` for those three. `dump` works with
`board.py --save`; interleave its I and Q halves into pairs to replay it.

### Receiving a run

`host/rx.py` runs `rx`, or `upload` and `replay`, reads the stream and checks every frame
against the transmitter's model: the xorshift32 stream with its station header, or with `--ts`
the TS test input. It exits 1 unless every frame from the first decoded one to the last is
present and exact. Its defaults are `--rate 1m`, `--mhz 2420`, `--gain 32`, `--seconds 30` and
`--callsign N0CALL`.

Start the receiver, then key the transmitter about 2 s later from a second shell. In the records
the receiver's run ends before the transmitter's data does
([sweep](../results/2026-10-23-rx-sweep/README.md)):

```sh
python3 host/rx.py 16 --rate 5m --seconds 45 --port RX_PORT --callsign YOURCALL
python3 host/board.py "dvb 2420 16 5m 55 0 128 --short" --port TX_PORT --timeout 90   # keys the transmitter
```

Replace `YOURCALL` with the callsign stored on the transmitter: `host/rx.py` rebuilds the
transmitter's payload, station header included, and checks each frame against it.

For TS, run `spiinput quad ts replay` on the transmitter first and add `--ts replay`.
`--replay FILE.sc8` uploads a capture and replays it with the radio off; `host/rx_twin.py`
makes synthetic ones. `--out FILE` appends the summary to a JSON-lines file. My bench joins the
two S3s with coax and 30 dB of attenuation, with peak 64 at 1 Msym/s, peak 128 at 5 Msym/s and
receiver gain 32 ([sweep](../results/2026-10-23-rx-sweep/README.md)).

## Memory after boot

Neither app touches the heap during a run. Each run takes its memory from regions reserved when
the firmware links or boots, and the next run hands all of it back at once.

| App | Region | Bytes | Holds |
|---|---|---:|---|
| Transmitter | Internal SRAM from 0x3fce0000, above the DAC banks | 38,672 | Encoder, bank-fill and DMA buffers; the first region with room wins |
| Transmitter | Internal SRAM from 0x3fcf0000, the DRAM half of a 32 KB data cache | 32,768 | The same |
| Transmitter | One PSRAM block, taken at boot | 393,216 | Cold tables, payload staging, dump buffers |
| Receiver | Static internal SRAM below the capture banks | 30,720 | Demodulator, clean-path FEC, TS deframer |
| Receiver | Internal SRAM from 0x3fce0000, above the banks | 28,672 | Symbol ring, three frame slots |
| Receiver | Static PSRAM | 98,304 | LDPC decoder, its frame, the CRC-32 table |
| Receiver | Static PSRAM store | 7,340,032 | An uploaded capture or a dump |

ESP-IDF's heap hooks count successful allocations and reallocations and frees of non-null
pointers; a separate callback counts failed allocations; zero-size calls and direct `multi_heap` calls are not
seen. `info` reports the count as `heap_after_boot`, and every transmitter completion record as
`heap_allocs_after_boot`. Through the transmitter commands of the run-memory record, a 10 s keyed run among them, they counted none
([TX run memory](../results/2026-10-23-tx-run-memory/README.md)). Through five receive runs, a
dump and `membench`, none either ([RX without Wi-Fi](../results/2026-10-23-rx-no-wifi/README.md)).
A few commands outside the runs still use the heap: `callsign`, whose flash write made 2
allocations and 2 frees; `spiinput`, for the SPI driver and CRC table; `spirx`, when it swaps
wires; and `spisend`, which sets up and frees its SPI master each time.

## Keyed commands

Connect the contained path first ([licence and containment](#licence-and-containment)).

**Length.** `dvb` accepts 1 to 540 s and `tx` 1 to 60 s. That is wall time with the opening and
closing Morse IDs inside it, and a run never runs over it. A request must be longer than two
IDs, or it fails before the radio starts: 11 s at least with N0CALL, whose ID lasts 5.22 s.
Requests longer than 540 s minus one ID end one ID early, because the firmware trims time for a
periodic ID that then never comes. A 540 s request with a 4.98 s ID keyed for 535.02 s
([record](../results/2026-10-23-tx-no-wifi-checks/README.md)).

**No stop button.** No command ends a run early, and closing `board.py` does not stop one. Keep
the RF path connected until the run's record arrives. A fill or deadline fault stops the run at
once, and then the closing ID may be missing.

**Drive.** The [keyed table](../results/2026-10-23-tx-table/README.md) used gain 0 with peak 128
at 40 MS/s and peak 48 at 80 MS/s, and every recovered frame was exact. An earlier 20 Msym/s run at
peak 128 overdrove the radio and decoded nothing ([measurements](measurements.md#over-the-air)). Gain is an
uncalibrated index, so output power is unknown.

**Filter.** Above 20 MHz of occupied bandwidth, which means `20m`, `26m667` and `33m333`, the
transmitter selects the radio's wide transmit filter, and otherwise the narrow one. The record's
`filter` field holds the register readbacks. Readbacks describe a configuration, not a measured
passband or emission mask.

**No Wi-Fi.** Neither app starts Wi-Fi. At boot `radio_power_up()` (`firmware/components/platform`)
powers the modem, enables the PHY under its Bluetooth modem bit, which runs the full calibration
once, and holds it there. Before each keyed run the transmitter makes one PLL and temperature
tracking pass, about 44,000 cycles (0.18 ms), the call Wi-Fi used to make every second
([no Wi-Fi](../results/2026-10-23-tx-no-wifi/README.md)). Wi-Fi had been costing frames: with
the transmitter's Wi-Fi up, 2 of 51,791 frames failed at the S3 receiver, and with it stopped
before keying (PHY-solo), 0 of 103,453 ([PHY-solo](../results/2026-10-23-tx-phy-solo/README.md)).
In a 535 s keyed run with only the one tracking pass, six 0.5 s bursts sampled from start to end
recovered 355 frames, every one exact; best-frame MER spanned 17.2 to 18.2 dB and moved 0.2 dB
from the first burst to the last ([record](../results/2026-10-23-tx-no-wifi-checks/README.md)).

`tx` plays a 40-sample pattern on repeat. The frequencies below are at 40 MS/s and double at
80 MS/s.

| SIGNAL | What plays |
|---|---|
| `tone` | +1 MHz, amplitude 256 DAC steps |
| `tone1` to `tone511` | +1 MHz at that amplitude |
| `low` | +1 MHz, amplitude 64 |
| `neg` | -1 MHz, amplitude 256 |
| `two` | +1 and +3 MHz, amplitude 128 each |
| `ionly`, `qonly` | A 1 MHz cosine on I only, or sine on Q only, amplitude 256 |
| `zero` | Nothing: the carrier leak alone |
| `dci`, `dcq` | DC of 200 on I, or on Q |

## Station identification

`callsign CALL` stores 3 to 23 characters in the S3's nonvolatile storage (NVS): letters, digits
and single slashes, at least one letter and one digit, no slash at either end, uppercased.
`callsign` reads it back and `callsign clear` erases it. That is a syntax check. It cannot
verify a licence. Keyed commands refuse to run without a callsign, and so does a board whose
stored callsign is unreadable or invalid at boot.

The examples on this page use N0CALL, a placeholder for radio-off tests. A licensed operator
stores their own assigned callsign, with any indicator their licence requires, before keying.

```sh
python3 host/board.py "callsign N0CALL"
```

Every keyed run starts and ends with the callsign in 20 WPM Morse. The timing follows
[ITU-R M.1677-1, Annex 1](https://www.itu.int/dms_pubrec/itu-r/rec/m/r-rec-m.1677-1-200910-i!!pdf-e.pdf):
a dot is 60 s / (20 × 50) = 60 ms and a dash three dots, with 5 ms raised-cosine edges in 10 µs
steps and seven dots of silence before and after. The tone sits 1 MHz above the carrier at
40 MS/s and 2 MHz above at 80 MS/s, at amplitude 128 DAC steps. N0CALL is 87 dots long: 5.22 s
per ID. The ID repeats every 540 s of wall time (`firmware/components/station_id/station_id.c`).
A keyed run lasts at most 540 s, so it never reaches a repeat, and its two IDs are at most 540 s
apart. Only radio-off `dvbpace` runs, which can carry 600 s of payload, reach one.

Every synthetic frame starts with a 40-byte header: `ESP-DVBS2 ID=` and the callsign, padded
with spaces, ending in a newline. External SPI and USB payloads go out untouched, with no header,
and the Morse IDs still run. Dumps contain no Morse ID, and without a stored callsign no header
either. To check the ID with the radio off and nothing written to flash:

```sh
python3 host/board.py "iddump N0CALL 80" --save id.iq
python3 host/morse.py id.iq --rate 10000 --tone 0 --expect N0CALL
```

The 10 kS/s dump aliases the tone to 0 Hz on purpose, so `morse.py` reads its envelope; it is
not a spectrum. The radio-off ID checks are in the
[archived station-ID record](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-station-id/README.md).
The records here measure the ID tone's level on a B200
([no-Wi-Fi checks](../results/2026-10-23-tx-no-wifi-checks/README.md)). None decodes the ID
from RF or measures its start and end transients.

The US rule requires identification at the end of a communication and at least every ten
minutes, and limits automatic CW identification to 20 WPM
([47 CFR §97.119(a) to (c)](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-B/section-97.119),
checked 2026-10-17). The operator remains responsible for the rules that apply.

## Licence and containment

Key the radio only with a valid amateur licence and an assigned callsign stored on the board.
Check your jurisdiction's band allocation, licence privileges and permitted emissions. The whole
occupied bandwidth, plus the ID tone 1 or 2 MHz above the carrier, must fit inside your
authorized band. In the US, see 47 CFR
[§97.301](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.301)
for licence and band privileges,
[§97.303](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.303)
for sharing restrictions,
[§97.305](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.305)
for permitted emissions and
[§97.307](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.307)
for emission standards, and check local rules and band plans too. The firmware's callsign check
and the example scripts' `--key` do not establish legal authority.

The console accepts integer center frequencies from 2000 to 2450 MHz for conducted tests. That
range is a test range, not a band plan. A B200 decoded the transmitter at 2000, 2025, 2067,
2110, 2200, 2245, 2290 and 2420 MHz ([S-band](../results/2026-10-20-s-band/README.md),
[lower edge](../results/2026-10-20-band-followup/README.md)). Lock, power and signal quality
across the rest of the range are uncharacterized.

Contain the signal in one of three ways: a dummy load; coax through at least 30 dB of
attenuation into the receiver; or a closed Faraday box with both devices inside, antennas only
inside the closed box. A box is not proof of zero leakage. On a board with a U.FL connector, use
a U.FL-to-SMA pigtail, attenuators and coax. Record the actual attenuation and the whole path,
and do not count unknown cable loss. Check the receiver's input-power limit and bandwidth before
connecting it. The S3-to-S3 records used 30 dB
([sweep](../results/2026-10-23-rx-sweep/README.md)) and the B200 keyed table 40 dB
([keyed table](../results/2026-10-23-tx-table/README.md)). The
[contained RF check](../examples/over-the-air/README.md) plans and dry-runs without a
device and keys only when given `--key`.
