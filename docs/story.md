# How it got here

The [README](../README.md) shows where the transmitter stands now. This page is the path to it:
what it measured and what went wrong. Raw links point to the preserved development archive.
All runs used one ESP32-S3 at 240 MHz.

![Payload rate at each development milestone, with its mode and symbol rate](img/rate-history.png)

Figures use [recorded points with per-point provenance](measurements/plot-data.json).

## The budget

The S3's radio contains a test engine that plays a 16,380-sample bank from internal RAM into
its I/Q DAC at 40 MS/s, looping until the CPU switches it to the other bank. After the overlap
the engine needs, a freshly filled bank is due every 356.7 µs, or 85,608 CPU cycles. Everything
else in this project is measured against that number.

The first benchmark asked whether one core could even write that fast. Copying 8-sample rows
from a precomputed table filled a bank in 179 µs, half the deadline. Building each sample from
separate I and Q tables in plain C took 401 µs and missed. PSRAM read at 59 MB/s against the
160 MB/s the DAC consumes, so samples have to be made in internal RAM, and PSRAM can only hold
payload. ([memory](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-memory/README.md))

## Coding and shaping

The first C encoder was plain bit-level loops. All 140 output CRCs (14 MODCODs, two input
types, five frames each) matched the Python reference. An 8PSK 2/3 frame took 2.35 M cycles,
which covers QPSK at 1 Msym/s with 27% of a core; 8PSK at 5 Msym/s needed it 2.3 times faster.
([encoder](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-encoder/README.md))

Meanwhile the Python reference's waveforms went through LeanDVB, an independent receiver:
40 host loopbacks, every payload recovered bit for bit.
([decode](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-decode/README.md))

The same idea as the [RP2350 version](https://github.com/Rivercraft911/RP2350_IQ_Benchmark):
precompute every output the shaping filter can produce. A 9-symbol root-raised-cosine pulse is
split into three 3-symbol windows, and each output sample is the sum of three table words plus
one XOR. Plain C took 15 cycles per sample. The S3's 128-bit PIE vector instructions brought
it to 1.78: 121 µs per bank, 34% of a core at 40 MS/s. All 18 board checksums matched the
Python model. ([shaper](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-shaper/README.md))

## First RF

The first way of keying the radio, forcing the front end on directly, radiated, but only the
I branch reached the mixer: a test tone came out double-sideband on a carrier leak stronger
than the signal. Starting the vendor's CW test tone, stopping it and moving only the PLL (a
method from esp-sdr-zephyr) brought both branches alive. Pinning the gain afterwards made the
level repeat within 0.5 dB. A +1 MHz tone then showed its image at -35 dBc and the carrier leak
near -45 dBc on an E200 SDR. In 45 keyed runs no bank fill was late: of 1,070,892 switches,
0.3% landed after their nominal time, all inside the overlap that both banks share.
([radio](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-radio/README.md))

## The whole chain on the chip, radio off

`dvbdump` runs the transmitter exactly as it would transmit, bank switches included, but sends
the samples over USB instead of starting the radio. LeanDVB decoded four QPSK code rates at
three drive levels with zero bit flips, and the 40 MS/s dump measured 47.0 dB MER against the
Python encoder. ([dry run](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-dryrun/README.md))

The transmitter stopped looping eight frames. Core 0 encodes a non-repeating payload frame by
frame into a ring, and core 1 shapes it into the banks. In 27 minutes of paced runs, 4.5 million
bank fills all met their deadline, and LeanDVB recovered contiguous bit-exact slices of the
stream. When the encoder was slowed on purpose, filler frames kept the stream valid.
([stream](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-stream/README.md))

## Keyed, in a shielded box

Station identification came first. The firmware refuses to key without a stored callsign,
sends 20 WPM Morse at the start and end of a run and every nine minutes, and puts the callsign
in every synthetic test frame. ([station ID](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-station-id/README.md))

The first keyed attempt stopped before keying with `ESP_ERR_NO_MEM` in Wi-Fi start-up: the
encoder context and station ID had left 8.5 KB of internal RAM. Shrinking Wi-Fi's buffers and
moving its code and allocations out of internal RAM raised idle free RAM from 69.0 to 86.8 KB.
The next run sent 8PSK 2/3 at 5 Msym/s, 9.68 Mb/s, for 30 s inside a lab Faraday box: 84,105
bank fills, none late, smallest slack 6,390 cycles. Nothing received it, so this shows the
transmit path running, not signal quality.
([keyed run](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-box/README.md))

## 10 Msym/s fits

Three changes made the 10 Msym/s fill fit, after a cycle profile of every stage. Frames stay in
internal RAM packed two symbols per byte, so neither core touches PSRAM per symbol. The 2,048
samples both banks share are kept in a cache instead of being shaped twice. At 10 Msym/s the
ring holds three frames. The longest fill fell from 98k-112k to 57k cycles, a third of the
deadline to spare, and the encoder in the stream from about 700k to 257k cycles per frame. The
dumps stayed byte-identical. ([build-out](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-buildout/README.md))

Keyed in the box, 8PSK 2/3 and 5/6 ran at 10 Msym/s, 19.4 and 24.2 Mb/s, for 30 s each with
every deadline met. ([keyed at 10 Msym/s](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-10m/README.md))

## 20 Msym/s

The last factor of two needed changes to the fill, encoder and bank schedule.
([fill](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-next2/README.md), [20 Msym/s](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-20msym/README.md))

- **Fill:** samples already finished in the bank overlap are reused instead of shaped twice,
  and a 554-byte symbol cache that was shifted with ROM's byte-by-byte `memmove` became 10
  bytes. The 4 samples/symbol bank went from 57k to 44k cycles.
- **Encoder:** with its workspace and BCH table moved into internal RAM, flash misses fell to
  zero and an 8PSK 2/3 frame went from 262k to 167k cycles.
- **Bank geometry:** the lead and overlap had been inherited from another project and never
  measured, and the timing pointed to the 1 kHz OS tick on the fill core. With
  interrupts masked through each switch and a 512-sample overlap at 80 MS/s, a bank is due
  every 47,220 cycles instead of 42,804.
Keyed in the box, 8PSK 2/3 and 5/6 ran at 20 Msym/s from the 80 MS/s DAC, 38.7 and 48.4 Mb/s,
for 30 s each with every deadline met.
([keyed at 20 Msym/s](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-20m/README.md))

![Encoder cycles across the 8PSK optimisation rounds and the later keyed QPSK reserve work](img/encoder-history.png)

## QPSK at 26.67 Msym/s

Four-symbol QPSK tables and a packed ring brought the complete 9/10 stream to
80/3 Msym/s. The radio-off run's worst bank was 39,139 cycles against 47,220;
all samples decoded bit for bit. Keyed in the box for 30 s, every deadline held,
carrying 46.6 Mb/s. This still does not establish receiver decoding over RF.
([radio off](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-qpsk-goal/README.md),
[keyed](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-keyed-qpsk/README.md))

## 40 MHz

The moonshot was the S3's widest Wi-Fi channel: 40 MHz occupied, 100/3 Msym/s at roll-off
0.20, 2.4 samples per symbol. A fill helper on
the second core kept fighting the encoder. A queue of precomputed table offsets rendered
banks fast but cost core 0 too much. The adopted **sign windows** treat QPSK's I and Q
as two BPSK streams so each four-sample vector comes from small sign-indexed tables and one OR.
Sign windows won once the mapper wrote sign histories directly. The last bugs were a truncated
USB report that looked like a hang and a mapper read through the flash cache.

Keyed in the box, QPSK 9/10 at 100/3 Msym/s ran for 30 s, with a 58.2 Mb/s data interval,
no late fill and no underrun, and every older mode still passed the regression gate. The encoder
uses 99% of a frame keyed, so the mode works without the 15% reserve. 8PSK at 40 MHz does not fit yet.
([40 MHz](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-moonshot/README.md))

The margin came the same day. The encoder now queues compact I/Q bit planes instead of sign
histories, core 1 pulls the sign windows straight out of them while rendering, and the encoder
is woken once per bank instead of once per page. Two frames are queued before playback so the
first data banks never wait. Keyed again in the box, the encoder averaged 198,879 cycles of a
239,630-cycle frame (17% spare, from 1%) and the longest bank fill was 39,548 of 47,220 (16%
spare): the 15% reserve, met on both cores. ([record](measurements.md#40-mhz))

The next morning brought a twist. Keyed runs had always looked about 47k cycles per frame
slower than radio-off paces, and everyone blamed the radio. It was the test payload: keyed
`dvb` runs generated fresh pseudo-random bytes for every frame, while paces copied a prepared
one. Given the same prepared payload, keyed and radio-off runs agreed, 150,667 against 152,754
cycles per frame, so keying costs core 0 essentially nothing. With real input the 40 MHz mode
has 37% encoder headroom. ([records](measurements.md#40-mhz))

![Longest bank-fill time and its deadline at the development milestones, including the failed 10 Msym/s attempt](img/fill-history.png)

Somewhere along the way the $5 chip outgrew the SDR on the bench: the E200's AD9363 sees at
most 20 MHz, so watching the whole 40 MHz signal takes a wider receiver.

## 8PSK at 36 MHz

The next step was 8PSK at 26.67 Msym/s. Three bits per symbol promised more data
than QPSK's 40 MHz mode, in a narrower channel. It began with a cache storm:
the encoder's flash tables and the renderer kept evicting each other's working
sets. Resident tables and compact queued descriptors made the encoder fast
enough. Standard dummy frames gave a late encoder somewhere to recover without
breaking the data sequence.

But the fastest encoder still spent 22% of its airtime on dummies. Models said it
should keep up. A page sweep finally showed what was missing: at 46 pages it
inserted 17,602 steady dummies in 10 s; at 50, 5,180; at 54, none. A data frame
needs 44 pages. Ten spare pages let the next frame's burst of mapping overlap
playback. The queue depth, not the mean encoder time, set the limit. Sharing the
table store and moving cold data to PSRAM made 54 pages fit with Wi-Fi running.

Keyed in the closed box for 30 s each, all three rates ran without a late fill
or a steady dummy: 51.6 Mb/s at 2/3, 64.6 at 5/6, and **69.8 at 9/10**. The longest
bank fills in the merged firmware were 39,419, 38,803 and 38,963 cycles against
47,220, and at least 27,731 bytes remained through Wi-Fi startup. Restoring the
slower modes' RAM cost about 9k encoder cycles per frame. 9/10 still has 20.5%
encoder spare and meets the 15% reserve on both cores; 2/3 and 5/6 run at full
rate with 14% and 12% encoder spare. The chip's
dumped samples decoded in LeanDVB; receiving the signal over RF is still ahead.
([keyed records](measurements.md#8psk-at-36-mhz))

## Now

Payload now arrives over [SPI or USB](spi-input.md), a [live-video example](../examples/live-video/README.md)
carries a camera's transport stream, and an [over-the-air check](../examples/over-the-air/README.md)
waits for an E200 in a bigger Faraday box: the next measurement is a receiver decoding the signal
over the air. 8PSK now reaches 26.67 Msym/s (36 MHz), and 40 MHz stays QPSK.
The current [implementation](how-it-works.md), [evidence](measurements.md) and
[radio-off example](../examples/self-check/README.md) are the starting point. All development
records remain in the [archive tag](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).

## Over the air

The E200 finally went into the closed Faraday box with the S3, receive-only and running
Pluto firmware. At 2420 MHz, QPSK 1/2 at 1 Msym/s came back as 587 frames, every payload
byte matching the host model, at 16.7 dB MER. QPSK 3/4 at 5 Msym/s followed, then a
124-frame burst at 10 Msym/s with zero LDPC flips. The samples had made it through the radio.

At 80 MS/s, turning it up made it worse. QPSK 9/10 at 20 Msym/s would not decode at
peak 128, even though the chip's samples were correct. The radio was being overdriven.
Peaks 32–64 decoded; 48 was best, at 14.2 dB MER. The digital tests had never exercised
that non-linearity.

There was another limit after that. A ±30 MHz chirp, received with the E200 tuned to
2405, 2420 and 2435 MHz and stitched from the middle of each capture, exposed the S3's
transmit filter: about 20 MHz wide, down 3 dB at ±10 MHz and about 20 dB by ±14 MHz.
The 36 and 40 MHz modes still meet their deadlines and generate correct samples, but
the filter cuts their edges away. Neither decodes over the air yet. A wider HT40
setting is the next investigation.

VCM made it through too: 2,936 frames switching QPSK code rates at 5 Msym/s, then
3,517 frames and 21 MB switching between QPSK and 8PSK every frame, byte-exact against
the host model. ([runs, plots and provenance](measurements.md#over-the-air))
