# The receiver

A second ESP32-S3 captures the first one's signal through its own radio and decodes every frame
on the chip. In a sweep of all 19 short-frame modes the transmitter sends, 30 s each, it decoded
223,153 frames, every one exact, none lost, no relocks, from 0.83 Mb/s (QPSK 1/2 at 1 Msym/s) to
12.64 Mb/s (8PSK 8/9 at 5 Msym/s) ([sweep](../results/2026-10-23-rx-sweep/README.md)). That was
over coax with 30 dB of attenuation at 2420 MHz, with one pair of boards and one receive gain.

The app is `firmware/rx`: capture, the receive tasks, replay and the console in `main/`, the
demodulator, FEC and TS deframer in `firmware/components/dvbs2rx`, which also builds on the host
for the tests. The overview and checks are in [How it works](how-it-works.md).

```
core 1: dump banks, 16 MS/s -> int8 I/Q -> matched filter at each symbol time
        -> header and pilot blocks: timing, carrier, DC, leak -> raw data symbols -> symbol ring
core 0: release: DC, derotation, leak, PL descrambling, demapping -> hard and weak bits
        check: re-encode -> BCH and BBHEADER -> USB record (-> TS deframer -> egress)
        decode (frames that fail the check): layered min-sum LDPC, state in PSRAM
```

## The radio, without Wi-Fi

The dump engine is part of the Wi-Fi baseband, but the receiver never starts Wi-Fi and links no
Wi-Fi code. At boot `radio_power_up()` (`firmware/components/platform`) powers the modem domain,
enables the PHY under the Bluetooth modem bit, which runs its full calibration as for a
Bluetooth-only application, and keeps the Wi-Fi clock domain on for the dump engine
([no Wi-Fi](../results/2026-10-23-rx-no-wifi/README.md)).

Each run then sets bit 1 of `SYSCON_WIFI_BB_CFG_REG`. A running Wi-Fi sets it, stopping Wi-Fi
clears it, and without it the dump holds a constant; a diff of the radio registers with Wi-Fi up
and stopped found it. A second diff, of 3,039 PHY, baseband and analog registers after the same
tuning and gain setup, found the radio in the same state with or without Wi-Fi, apart from one
16-byte front-end table one step lower in every byte and one status bit
([Wi-Fi A/B](../results/2026-10-23-rx-no-wifi-ab/README.md)). The receive and transmit filter codes
were equal.

## Capture and handoff

The dump engine writes 10-bit I/Q at 16 MS/s into a 16,384-word ring in whichever of three
64 KiB SRAM banks it owns. A timer interrupt on core 1 moves ownership to the next bank every
12,288 samples (768 µs, 184,320 cycles at 240 MHz): it wakes 15 µs early, spins to the exact
write index and re-anchors, so handoffs land within a few samples. The capture task then works
through the released banks and may trail by one unit. If it trails by two, the interrupt takes
back the newest bank it has not started, and the loss reaches the demodulator as a jump in the
sample index. In every recorded run since the 5 Msym/s fixes, no unit was dropped.

The register map for all of this, and the idea of rotating the bank select under a running
engine, come from esp-sdr-zephyr and ESP-SDR ([credits](prior-art.md)).

## Front end

Each 10-bit word becomes int8 I and Q, `floor(s10/4)`, at 1.55 cycles per sample. A
root-raised-cosine matched filter then runs only at symbol times, from a bank of fractional-delay
coefficient sets:

- **1 Msym/s, 16 samples per symbol:** 96 taps at 1/64-symbol resolution, from 64 coefficient
  sets of seven 16-byte vectors, about 61 cycles per symbol on PIE.
- **5 Msym/s, 3.2 samples per symbol:** 32 taps (two aligned 16-byte vectors) at 1/32-sample
  resolution. Five symbols span 16 samples, so each group of five shares one window: 15.6 to
  16.3 cycles per symbol on PIE.

**The ISI fold.** At 5 Msym/s a 3-tap equalizer fitted to the real symbols found -0.034 to
-0.037 at ±1 symbol, real, symmetric and stable across frames and captures (-0.004 at
1 Msym/s): the analog filters smear each symbol of a 6.75 MHz signal slightly into its
neighbours. The 5 Msym/s coefficients cancel 0.035 at no cost per symbol, and data MER on five
recorded dumps rose from 23.2-24.3 dB to 27.2-29.1 dB
([5 Msym/s with margin](../results/2026-10-23-rx-5m-margin/README.md)).

**The wrap.** The 5 Msym/s filter plans each batch of up to 256 symbols from times in 1/32
sample, and in 32 bits those wrap every 2^27 samples, 8.39 s. A batch that straddled a wrap took
it for a window advance and filtered about 252 symbols with the wrong coefficients. Two frames
failed that way. The plan is now worked out in 64 bits, and 441,550 frames since decoded with
none failed ([Wi-Fi A/B](../results/2026-10-23-rx-no-wifi-ab/README.md#the-failed-frames)).

## Acquisition

Until it locks, every unit runs the header search, and the search has to fit the 184,320 cycles a
unit lasts on any input: noise, the Morse ID, or data. It correlates the signs of matched-filter
outputs against the start-of-frame and PLS code; a PIE kernel forms the signs of 64 candidates
in 408 cycles ([demodulator](../results/2026-10-22-rx-demod/README.md)). At 5 Msym/s there
is a candidate every 1.6 samples, five times as many per sample, so the search builds its half
sums 320 at a time without divisions and covers 4,096 of every 16,384 samples, at an offset that
moves from block to block.

**The eight-sign prefilter.** A candidate must first match the 8 newest differential signs of the
header. It used to be 3. The Morse ID is a tone, a tone gives the same differential sign
throughout, and MODCOD 16's newest three header terms happen to be equal, so during the ID every
candidate passed and the search spent 247k cycles per 184k-cycle unit. With 8 signs, the search
averages 59k cycles per unit on noise and 110k at worst
([5 Msym/s with margin](../results/2026-10-23-rx-5m-margin/README.md)).

**The lock.** The search does not lock on the header it finds. It waits for the next one, a
frame later, and locks there at the clock the carrier offset implies. Both radios run from
40 MHz crystals, so the transmitter's symbol clock against the receiver's sample clock equals the
carrier offset divided by the carrier frequency; a float fit agreed within 0.04 ppm on 15
captures and within 0.01 ppm on 13 of them
([clock against carrier](../results/2026-10-22-rx-demod/README.md#clock-against-carrier-offset)).
The lock unit is the most expensive one, 170k to 225k cycles at 5 Msym/s with capture service,
and one unit of capture lag absorbs it.

**Dummy frames.** A transmitter whose input runs dry sends dummy PLFRAMEs: a header with MODCOD 0,
then 3,240 symbols of unmodulated carrier and no pilots (EN 302 307-1 §5.5.1). Acquisition locks
only on a frame's header; when the header it lands on is a dummy's, it hops 3,330 symbols to the
next header and tries again.

## Tracking

Once locked, every header and pilot block is a known block: each one updates the timing, carrier
phase and frequency, DC and the transmitter's carrier leak. The leak sits about 17 dB below the
transmitter's own signal, and taking it out after derotation raised MER from about 16 dB to about
29 dB ([commit](https://github.com/Rivercraft911/esp-dvbs2/commit/ec47b06a2d5923f337be6108b8e37a34a0b0a222)). On captures where the
demodulator measured 26 to 31 dB, LeanDVB, which does not remove the leak, reported 7 to 16 dB
([demodulator](../results/2026-10-22-rx-demod/README.md)).

A dummy frame's header is one more known block: the matched filter runs through its carrier,
the DC estimate goes on, and the next header is expected 3,330 symbols later. Over the S3 link the
receiver rode through 95,810 dummy frames from a transmitter starved of input without losing lock
([TS link](../results/2026-10-23-ts-link/README.md)).

Core 1 keeps each data symbol's raw matched-filter output in a ring of 4,096 symbols (16 KB,
0.82 ms at 5 Msym/s). It queues a run of data symbols once the known block after it is measured,
so the carrier phase is known at both ends of the run.

## Core 0: release, check, decode

Three tasks share core 0:

- **Release** (highest priority) takes off DC, derotates with the phase interpolated between the
  known blocks, takes off the leak, descrambles and demaps. Each code bit becomes a hard bit, its
  sign, and a weak bit, set when it lies less than half the noise-free distance from its decision
  boundary. For 8PSK it writes each symbol's three bits to three bit planes, which undoes the
  column interleaver. A frame of hard and weak bits needs 4 KB instead of 16 KB, so frames stay
  in internal RAM. The cost, measured on the host against the same decoder fed int8 soft values,
  is 0.5 dB at QPSK 3/4 and 8/9 and over 2 dB at 1/2
  ([decoder near threshold](../results/2026-10-24-rx-fec-curve/README.md#where-the-db-go)).
- **Check** re-encodes each frame's hard bits with the transmitter's own fast BCH and LDPC encoder
  and compares parity: 31.6k cycles for a clean QPSK 3/4 frame
  ([commit](https://github.com/Rivercraft911/esp-dvbs2/commit/32990841d2ad3f8822499dd570fe7ecc2c9cc52e)).
  If the check holds all three frame slots, release waits up to 2 ms for one before it drops a
  frame, because the first frames after a lock arrive in a burst while the first check runs with
  cold caches.
- **Decode** (lowest priority) runs a layered normalized min-sum LDPC decoder in int8 on frames
  that fail the check, one at a time. It uses the code's 360-column structure, 16 checks per PIE
  vector, and builds two-level beliefs from the hard and weak bits. For QPSK 3/4 short frames
  (MODCOD 7), with the 48 KB work area in PSRAM, an iteration takes 393k cycles; other MODCODs
  need different amounts of decoder memory
  ([commit](https://github.com/Rivercraft911/esp-dvbs2/commit/d2c33ea94fa3ab788602c8cf5d83ea9cca9ab1bb)),
  for at most 8 iterations with an early stop. 8PSK MODCODs use the code of the QPSK MODCOD with
  the same rate.

On a clean link the decoder rarely runs. In the 19-mode sweep no frame failed its check
([sweep](../results/2026-10-23-rx-sweep/README.md)). In the 2.4-hour live demo one frame of
5,725,408 did, in run 31, and the decoder repaired it in 2 iterations; in TS mode a repaired frame
arrives too late for the stream, which is that run's one continuity error
([live demo](../results/2026-10-23-live-demo/README.md)).

Near threshold, measured radio off with noisy frames demapped by the receiver's own kernels, the
decoder reaches 1% frame errors about 1 dB above the DVB-S2 ideal threshold (Table 13) for QPSK
3/4 to 8/9 and 8PSK 8/9, and 1.5 to 3.5 dB above it for the lower rates, with no wrong payload in
72,000 frames. Speed limits it first. Everything the decoder touches is in PSRAM, so a frame with
any bit error takes at least 2 iterations and 7.4 to 9.4 ms, and a damaged frame that arrives
while the decoder is busy is lost. Replaying noisy QPSK 3/4 captures through the receiver, 43% of
frames were lost at 8 dB and none at 14 dB at 1 Msym/s, 38% at 12 dB and none at 14 dB at
5 Msym/s; the code alone decodes from about 5 dB
([decoder near threshold](../results/2026-10-24-rx-fec-curve/README.md)).

**Records.** Each frame becomes a 20-byte record over USB: sequence, carrier offset, MER, length,
LDPC iterations and failing checks (`firmware/rx/main/receive.h`). At 1 Msym/s the payload follows.
At 5 Msym/s the payload outruns the S3's full-speed USB port (12 Mb/s signalling), so the
payload's CRC-32 follows instead and `host/rx.py` compares it with the model frame the sequence
number implies. A failed frame carries its header's sample index and its 2,025 bytes each of hard
and weak bits, for diagnosis.

## Transport streams

In TS mode (`rx ... ts`) the check task also turns each good frame back into 188-byte MPEG
transport stream packets (`dvbs2rx/rx_ts.c`, EN 302 307-1 §§5.1.4 to 5.1.6). The transmitter
replaced each packet's sync byte with the CRC-8 of the packet before it and cut the stream into
data fields without regard to packet boundaries, so the deframer carries a packet across frames,
checks each packet when the next one's first byte arrives, sets the transport_error_indicator on
a mismatch, restarts at SYNCD after a lost frame, and checks continuity counters for up to 16
PIDs. Packets go to a TS egress (`firmware/rx/main/ts_egress.h`); with `pattern`, each is checked
against the transmitter's test input on the chip instead. Frames the LDPC decoder repairs arrive
too late for the stream and count as lost.

Over the S3 link 1,738,344 packets crossed in 10 runs at up to 12.64 Mb/s, every one exact, with no
CRC-8 errors and no lost packets. The deframer costs 1.88k cycles per packet at QPSK 3/4 and 2.72k
at 8PSK 8/9 on a busy core 0, 3.6% and 9.5% of the frame
([TS link](../results/2026-10-23-ts-link/README.md)). The egress had no sender in those runs;
the packets were counted and checked, not forwarded.

The deframer now checks each whole packet with the transmitter's PIE CRC-8 kernel
(`dvbs2_pie_crc8_packet`) and keeps the byte table for packets at a frame's edges. Measured the
same way at 8PSK 8/9, it costs 23.5k cycles a frame, against 25.6k in the TS link runs and 57.0k
before the duplicate fingerprint was made lazy. In a 60 s run without the pattern check, core 0's
counters summed to 74.5% of the frame period, with all 47,742 frames and 449,231 packets exact
([TS cost](../results/2026-10-23-rx-ts-cost/README.md),
[TS CRC-8](../results/2026-10-23-rx-ts-crc/README.md)).

## Ethernet out

With `eth` (`rx ... ts eth`) the packets leave through a WIZnet W5500 on SPI2
(`components/w5500`, written from the W5500 datasheet, and `firmware/rx/main/eth_egress.c`). The
W5500 runs UDP and IP in hardware, so the receiver uses no software network stack and the egress
needs no heap. The checker copies each packet into a 4,096-packet queue in PSRAM and never waits: a
packet that finds the queue full is dropped and counted. A sender task on core 0, above the
checker and below the release, sends datagrams of seven packets (1,316 bytes) on two sockets in
turn, so the chip holds the next datagram while one is on the wire. It drives GP-SPI2 and a GDMA
channel directly at 26.67 MHz (`w5500_spi_direct.c`) rather than through ESP-IDF's spi_master:
76 µs of CPU per datagram against 281 µs, paced at 5 Mb/s
([bring-up](../results/2026-10-23-eth-bringup/README.md)).

The module sits on the XIAO's 3.3 V supply and kept losing its link at 100 Mb/s, so the
receiver forces 10 Mb/s full duplex (`eth phy 10f`). That carries up to 9.46 Mb/s of TS, enough
for the full 9 Mb/s camera mux: 432,975 datagrams in a 540 s run with none dropped
([full quality](../results/2026-10-23-eth-full-quality/README.md)).

The monitor stream goes to `host/monitor.py`, the laptop's dashboard: the receiver's statistics
five times a second, and sample snapshots and corrected symbols ten times a second (`mon_pack.h`,
rates set by `mon`). It takes a turn on the same two sockets whenever no TS datagram waits; a
third socket made the chip lose and reorder TS datagrams. The sender makes those copies itself on core 0; done on
core 1 they cost the demodulator frames. With the monitor on, an 8 Mb/s camera run had no frames
or demodulator runs dropped ([monitor](../results/2026-10-23-eth-monitor/README.md)). The
transmitter's status lines reach the same dashboard through `host/board.py --monitor`.

The longest test is the live demo: 16 runs of 540 s, 5,725,408 frames decoded with none failed,
6,127,597 datagrams with none dropped. In five of the runs the release fell behind and lost
frames, 11 in the 2.4 hours ([live demo](../results/2026-10-23-live-demo/README.md)).

## Memory

Nothing the receiver runs touches the heap after boot. Each run takes its memory from regions
reserved when the firmware links and gives it back when the next run starts
(`firmware/rx/main/arena.c`, [arenas](../results/2026-10-23-rx-no-wifi/README.md#memory)):

| Arena | Contents | Largest use / size |
|---|---|---:|
| Internal RAM below the capture banks | demodulator, clean-path FEC, TS deframer | 29,760 of 30,720 B |
| Internal RAM just above the banks | symbol ring, three frame slots | 28,576 of 28,672 B |
| Static PSRAM | LDPC decoder state and work, the decoder's frame, the CRC-32 table | 71,488 of 98,304 B |
| Static PSRAM store | a replay's capture or a dump | 7 MB |

Queues, semaphores, the capture task's stack and the capture timer are made once at boot from
static storage; the timer is made on core 1, where its interrupt must run. Through 5 receive
runs, a dump and `membench`, ESP-IDF's heap hooks counted no heap call after boot, and 51,482
bytes of heap stayed free for ESP-IDF throughout. The hooks see successful allocations,
reallocations and frees of non-null pointers, and a callback counts failed allocations; zero-size
calls and direct `multi_heap` calls are not seen.

## Measured

From the sweep: 30 s per mode, coax and 30 dB at 2420 MHz, receive gain index 32, firmware
c3a1c2d on both boards ([sweep](../results/2026-10-23-rx-sweep/README.md)).

| Modes | Payload | Frames, all exact | Pilot MER, median | Core 1 per unit, mean / worst | Core 0, release + check |
|---|---:|---:|---:|---|---|
| QPSK 1/2 to 8/9, 1 Msym/s | 0.83 to 1.69 Mb/s | 19,705 | 30.2 to 30.3 dB | 78.2k to 78.4k / 131k to 134k | 7 to 8% |
| QPSK 1/2 to 8/9, 5 Msym/s | 4.15 to 8.45 Mb/s | 98,213 | 29.7 to 29.8 dB | 112.3k to 112.6k / 160k to 218k | 39 to 43% |
| 8PSK 3/5 to 8/9, 5 Msym/s | 8.46 to 12.64 Mb/s | 105,235 | 29.6 to 29.8 dB | 116.1k to 116.7k / 177k to 226k | 60 to 63% |

A unit lasts 184,320 cycles. Worst units include capture service and the lock; any over 184,320
were absorbed by one unit of capture lag, none dropped. Core 0 shares are wall clock, each
including preemption by the other task, so their sum is an upper bound. The receiver has run at
one frequency and one gain, over coax, between one pair of boards.

One problem is not settled. In one session, 12 of 27 runs, with every firmware, lost one burst of
about 950 frames (1.06 s): core 0 stopped taking frames for about a second while capture kept
lock and dropped no unit. The likely cause is the check task blocking in its USB record write
when the host stops reading; four later runs with a host that logs its read gaps had no burst,
so the cause is not established
([Wi-Fi A/B](../results/2026-10-23-rx-no-wifi-ab/README.md#not-settled)).

<p align="center"><img src="img/rx-5m.png" alt="What the receiving S3 sees at 12.64 Mb/s: spectrum, spectrogram and constellation" width="760"></p>

8PSK 8/9 at 5 Msym/s, captured by the receiving S3 itself and drawn from its demodulator's
output: 164 of 164 frames exact, data MER 28.9 dB ([figures](../results/2026-10-23-rx-figures/README.md)).
