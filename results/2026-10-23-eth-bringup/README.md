# W5500 bring-up and the first live video over Ethernet

**Result.** Live camera video went from the CM5 through the S3 transmitter, over coax, into the
S3 receiver, and out of the receiver's W5500 over UDP to the laptop's viewer. With the cameras
at 2.5 Mb/s each (6 Mb/s mux), a 540 s keyed run: 268,401 of 268,401 frames good, and the
egress sent all 1,972,729 TS packets in 282,235 datagrams with none dropped (peak queue 14 of
256). The laptop got all but 22 datagrams, and 736 of its packets (3.7e-4) arrived with a
corrupted first byte, mostly zeros. At the original 9 Mb/s mux the egress carried 8.28 Mb/s and
dropped 0.028% at its queue. The W5500 link holds only at a forced 10 Mb/s full duplex on this
bench: 100 Mb/s flaps.

Setup, 2026-10-23:

- Receiver: XIAO ESP32-S3 44:B1:76:B0:E5:54, firmware `firmware/rx` with the W5500 egress, direct
  GP-SPI2 backend (`CONFIG_W5500_DIRECT_SPI=y` over `sdkconfig.defaults`), except where a run
  says `esp-idf`. A W5500 module wired as in `w5500_eth.h` (SCLK 9, MOSI 43, MISO 44, CS 3),
  powered from the XIAO's 3V3, at SPI 26.67 MHz.
- Laptop: MacBook, UGREEN USB Ethernet adapter (AX88179B, en12), static 192.168.1.100/24. W5500
  192.168.1.3/24, destination 192.168.1.100:1234 (`eth ip`, `eth dest`, RAM only).
- Transmitter: XIAO 44:B1:76:B0:E5:8C, firmware 517c641, coax into the receiver (marked 40 dB),
  2420 MHz, PEAK 128, gain 0. Live input: the CM5's `pv-capture` (two IMX900, 2064x1552 at
  30 fps, x264) over single-line SPI at 16 MHz (`spiinput on ts`).
- Viewer: PigeonVision `pv view udp://0.0.0.0:1234`, recording what arrived
  (`--record-transport`).

## Link

PHYCFGR (W5500 DS p.42) sampled every 10 ms for 20 s by `eth watch 20`, after `eth phy MODE`;
en12's status from `ifconfig` every 0.5 s meanwhile:

| Mode | Link changes | Up | Longest up | Longest down | en12 active |
|---|---:|---:|---:|---:|---:|
| auto (PMODE pins) | 16 | 31.1% | 0.79 s | 1.63 s | 0 of 50 |
| 100f forced | 12 | 25.9% | 0.87 s | 3.63 s | 1 of 50 |
| 10f forced | 1 | 92.6% | 18.5 s | 1.48 s | 45 of 50 |

10f's one change is the link coming up 1.48 s after the PHY reset. The chip answers on SPI in
every mode (VERSIONR 4, address readback). The likely cause is the module's supply, the XIAO's
3V3 with no regulator on the module; a first module, cable and adapter never held a link
either.
A start now waits up to 3 s for the link after setting a PHY mode, so the first unicast SEND's
ARP does not time out (d72cbe1).

## `eth send` at 10 Mb/s

Numbered 7-packet datagrams for 10 s, received by `host/udp_ts.py --diag` under caffeinate.
CPU per datagram = (core_load - core_load_before) x us / datagrams; it includes the diagnostic's
own polling once a tick while paced.

| Backend | Offered | Board Mb/s | Laptop datagrams lost | CPU per datagram |
|---|---|---:|---:|---:|
| esp-idf | unpaced | 9.10 | 0 of 8,645 | (spins on FULL) |
| esp-idf | 9 Mb/s | 8.98 | 96 of 8,532; 47 of 8,529 | (spins on FULL) |
| esp-idf | 5 Mb/s | 5.00 | 0 of 4,750 | 281 us |
| direct | unpaced | 9.46 | 32 of 8,986 | (spins on FULL) |
| direct | 5 Mb/s | 5.00 | 0 of 4,750 | 76 us |

The wire's ceiling is 9.52 Mb/s (1,382 bytes a datagram at 10 Mb/s). The laptop's losses came in
one burst per run (datagrams 6,874 to 7,301 in the repeat), with SEND_OK for every datagram and
no input errors on en12. Heap at boot for the port: 948 B (esp-idf), 332 B (direct).

## RF check

8PSK 3/4 short, 5 Msym/s, 20 s of xorshift payload, receive gain index 32: 8,966 of 8,966
frames exact, pilot MER 24.3 to 25.8 dB, no capture drops.

## Egress

`rx 2420 32 SECONDS MODCOD 5m ts ... eth`: every TS packet the receiver hands on goes into a
256-packet queue and out in 7-packet datagrams (firmware/rx/main/eth_egress.h). Development runs,
QPSK 3/4 short 5m with the TS replay pattern (6.95 Mb/s), each fixing what the last showed:

| Firmware | Egress | Packets dropped | Cause and fix |
|---|---:|---:|---|
| d72cbe1 | 6.29 Mb/s | 4,010 of 46,357 | the chip sends one datagram at a time and starts the next only when called; a tick (1 ms) is less than a datagram's 1.106 ms on the wire. e78e51b: an esp_timer wakes the sender when it should be out |
| e78e51b | 6.32 Mb/s | 3,926 of 46,350 | the wake was timed from after the call that waited out the posted write; bd2b79a times it from the call that gave the SEND |
| bd2b79a | stopped | 44,162 | `spi` bus error: a bounded wait counted a preemption as SPI time; 7feee29 gives the register a last look |

The first live run (7feee29, sender at priority 10) carried about 6.9 Mb/s: the sender woke late
behind the release (12) and checker (11). f8f023d runs it at 13.

## Live video

| Run | Mux | Frames | Egress | Dropped at queue | Laptop |
|---|---|---|---|---|---|
| f8f023d, 330 s | 9 Mb/s (x264 4 Mb/s x2) | 241,163 good, 0 failed | 8.28 Mb/s, 259,529 datagrams all SEND_OK | 510 of 1,816,705 (0.028%), peak 256 | pv: about 1 continuity gap per second per video PID |
| f8f023d, 575 s (540 s keyed) | 6 Mb/s (x264 2.5 Mb/s x2) | 268,401 good, 0 failed | 5.4 Mb/s, 282,235 datagrams all SEND_OK | 0, peak 14, chip never full | 282,213 datagrams; 1,972,575 packets, 736 with a bad first byte (496 zero), 627 continuity errors |

Both receive runs: pilot MER about 25 dB (10th percentile 25.0, median 25.6 in the second), no
capture drops, no frames dropped, 0 heap after boot. The receiver left out null packets
(`nonull`: 48,410 and 103,089); its deframer counted 46 and 6 continuity errors in the stream it
received, from the source or its SPI link, not the radio (no CRC-8 errors). The transmitter had
no late fills in either 540 s run. The camera rate was lowered for the second run
(`vbv_bits` scaled with it, 1,250,000) because the first overflowed the queue.

The viewer's counts for the second run (`pv` record): 9,615 and 10,511 access units, 464 corrupt
packets, 10 and 44 discontinuities on the video PIDs and 4 on metadata. Its "other" count
(4,663) is not continuity on the other PIDs: the recording has 12, 13 and 5 errors on PAT, PMT
and SDT, and none on PID 0x0747, which the transmitter's own packets use.

## Open

- 100 Mb/s link (supply). At 10 Mb/s the 9 Mb/s mux leaves 7% headroom and the queue overflows.
- About 3.7e-4 of packets reach the laptop with a first byte other than 0x47, mostly zeros, only
  while the receiver runs (`eth send`: none in about 308,000 packets; the egress test runs had 6 and
  24). The receiver writes 0x47 itself and its CRC-8 found no errors, so the bytes change after
  the deframer. Zeros fit the SPI shifting out an empty FIFO (GDMA starved by the capture's
  memory traffic?) better than stale W5500 memory. To separate the causes: the TS replay pattern
  checked byte for byte at the laptop, at 26.67 and 13.33 MHz SPI, and with the esp-idf backend.
- 22 datagrams of 282,235 did not reach the viewer (SEND_OK for all); the `eth send` runs lost
  datagrams in bursts near the wire's ceiling too.
- The esp-idf backend in the egress is untested since the priority change.

## How it was run

    idf.py -B build -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;direct.defaults" build flash
                                          # firmware/rx; direct.defaults: CONFIG_W5500_DIRECT_SPI=y
    board.py "eth phy 10f"; board.py "eth ip 192.168.1.3/24"; board.py "eth dest 192.168.1.100:1234"
    board.py "eth watch 20"               # after eth phy auto, 10f, 100f
    caffeinate -i -s -d host/udp_ts.py --port 1234 --diag   # with board.py "eth send 10000 [MB/S]"
    host/rx.py 14 --rate 5m --gain 32 --seconds 26          # RF check, with "dvb 2420 14 5m 20 0 128 --short" on the TX
    host/rx.py 7 --rate 5m --ts replay --eth ...            # egress runs, TX "spiinput quad ts replay"
    rx 2420 32 575 14 5m ts nonull eth                      # live, via a small runner without a model check
    TX: spipins xiao; spiinput on ts; dvb 2420 14 5m 540 0 128 --short
    CM5: pv-capture --config capture.json (a copy of output/esp-live-20261019/capture.json with
         its own session_dir; the second run with bitrate 2,500,000, vbv_bits 1,250,000,
         mux_bitrate 6,000,000)
    Mac: pv view udp://0.0.0.0:1234 --no-browser --calibration calibration/bench-2026-10-02/calibration.json \
         --record-transport FILE

`runs.jsonl` holds every record: `phy` (eth phy and eth watch), `eth` (status, settings, eth
send), `laptop` (udp_ts.py), `rf`, `egress`, `tx`, `live` (receiver summaries with the egress
counters), `pv` and `pv_samples` (the viewer's /health), `recording` (udp_ts.py over the second
run's recording). The recordings themselves (0.5 and 0.37 GB) are not committed.
