# Transmitter without Wi-Fi

**Result.** Without Wi-Fi, every keyed QPSK 9/10 5 Msym/s frame that LeanDVB decoded from the
B200 was exact, in all six A/B runs, and all 22 radio-off paces kept their deadlines. Internal
RAM low-water marks rose by 19 to 37 KB. 8PSK 5/6 at 26.67 Msym/s now starts (it failed
with `ESP_ERR_NO_MEM` with Wi-Fi up), and 8PSK 2/3 and 9/10 at 26.67 Msym/s dropped from 1,376
and 802 steady dummy frames in 10 s to 0. The station ID tone is 1.4 dB weaker without Wi-Fi.
That difference is not explained. Output power is uncalibrated in either case. The 36 and
40 MHz modes could not be compared on air: Wi-Fi traffic on channels 1 and 6 near the bench
blocked B200 decoding with both firmwares.

Firmware: 74c9b06 (no Wi-Fi) against b003f26 (Wi-Fi up, the commit before). The keyed A/B
ran on the uncommitted tree that became 74c9b06 (`b003f26-dirty` in the records). That tree
had one extra console switch, which skipped the tracking call. The switch was removed before
the commit.

## What changed

At boot, `radio_power_up()` (firmware/components/platform) powers the modem domain, enables
the PHY under the Bluetooth modem bit and turns the Wi-Fi baseband clocks on. The PHY's
first full calibration runs at this point. The receiver has used the same bring-up since
1216b6e. Before each keyed run, the worker makes one `phy_param_track_tot(true, false)` call.
It takes about 44,000 cycles (0.18 ms; `phy_track_cycles` in the A/B records).
With Wi-Fi up, Wi-Fi's 1 s PLL timer used to make that call every second. A register diff on
the receiver showed that one call returns a 16-byte front-end table to its Wi-Fi-up values
([rx-no-wifi-ab](../2026-10-23-rx-no-wifi-ab/README.md)). The TX image shrank from 939 KB to
544 KB.

## Radio-off paces (10 s each, xorshift payload)

Set `pace-wifi` (b003f26) and `pace-no-wifi` (74c9b06) in `runs.jsonl`. Encoder means are
cycles per frame. "Min free" is `min_free_internal`, the internal-heap low-water mark in bytes.

| Mode | Wi-Fi: status | mean | dummies | min free | No Wi-Fi: status | mean | dummies | min free |
|---|---|---:|---:|---:|---|---:|---:|---:|
| QPSK 9/10 1m | OK | 716,725 | 0 | 72,375 | OK | 710,329 | 0 | 91,099 |
| QPSK 9/10 5m | OK | 161,718 | 0 | 51,667 | OK | 161,728 | 0 | 70,391 |
| QPSK 9/10 20m | OK | 129,286 | 0 | 44,663 | OK | 128,442 | 0 | 63,387 |
| QPSK 9/10 26m667 | OK | 129,133 | 0 | 61,367 | OK | 128,383 | 0 | 80,091 |
| QPSK 1/2 33m333 | OK | 180,502 | 0 | 29,023 | OK | 179,967 | 0 | 65,675 |
| QPSK 9/10 33m333 | OK | 177,450 | 0 | 27,815 | OK | 177,359 | 0 | 62,523 |
| 8PSK 9/10 20m | OK | 253,939 | 0 | 56,831 | OK | 247,357 | 0 | 76,023 |
| 8PSK 2/3 26m667 | OK | 191,797 | 1,376 | 24,675 | OK | 187,583 | 0 | 59,411 |
| 8PSK 5/6 26m667 | NO_MEM | | | 21,983 | OK | 196,271 | 2 | 56,723 |
| 8PSK 9/10 26m667 | OK | 183,910 | 802 | 24,387 | OK | 180,209 | 0 | 59,127 |

The other 12 modes in the records behave the same way. Encoder means moved by -3.7% to
+2.7%, except 8PSK 8/9 short frames at 5m, which moved by -12%. That is within the
layout-to-layout spread measured earlier
([tx-pins](../2026-10-23-tx-pins/README.md)), not a Wi-Fi effect. The 8PSK 26m667 frame
lasts 199,746 cycles. Its encoder means are still above the 169,784-cycle (15% reserve)
target.

## Keyed A/B at 5 Msym/s

`dvb 2420 11 5m 20 0 128` into the B200 through 40 dB of coax attenuation, at 10 MS/s and gain
40. Each run started from a fresh boot. Order: W N T T N W. W is Wi-Fi up (b003f26). N is
without Wi-Fi and without the tracking call. T is without Wi-Fi and with the tracking call
(as committed). Set `keyed-5m-ab`. Each run captured a 0.5 s ID burst at 2.0 s and a 1.0 s
data burst at 8.0 s. LeanDVB decoded the data burst. Each decoded frame was regenerated from
its recovered xorshift32 state.

| Arm | ID tone (dB) | image (dBc) | carrier (dBc) | frames | exact | wrong | MER median (dB) |
|---|---:|---:|---:|---:|---:|---:|---:|
| W | 63.9 | -38.2 | -25.7 | 128 | 128 | 0 | 14.5 |
| N | 62.7 | -63.2 | -24.0 | 105 | 105 | 0 | 15.3 |
| T | 62.7 | -56.5 | -29.9 | 134 | 134 | 0 | 18.1 |
| T | 62.3 | -39.8 | -28.7 | 126 | 126 | 0 | 18.9 |
| N | 61.8 | -46.2 | -26.5 | 130 | 130 | 0 | 12.4 |
| W | 64.3 | -62.0 | -28.0 | 137 | 137 | 0 | 18.0 |

The tone level is the only consistent difference. It was 63.9 and 64.3 dB with Wi-Fi against
61.8 to 62.7 dB without. The day before, a separate experiment build gave 63.9 and 63.9 dB
against 62.7 and 62.5 dB. The tracking call does not restore the level: T averages 62.5 dB,
N averages 62.25 dB. The PHY's maximum-power setting is not the cause either.
`phy_get_most_tpw()` reads 80 (20 dBm) without Wi-Fi, the same value Wi-Fi's start sets
(set `keyed-5m-max-power`).

Image, carrier and MER readings are not usable for this comparison. Wi-Fi bursts land inside
the 5 MHz band, around the ID tone's image and the carrier. One run's own MER spanned 11.6 to
19.2 dB. The frames lost between bursts (the `breaks` in each record) were lost to the same
bursts.

## 80 MS/s

Set `keyed-80ms`. Neither firmware decoded 8PSK 26m667 into the B200. QPSK 9/10 20m decoded
partly, with every decoded frame exact (W: 67, T: 9). A 50 MS/s spectrum with the
transmitter idle showed two signals: 802.11b-shaped bursts centred near 2411 MHz, at about
15% duty, up to 30 dB above the floor and 10 dB above this signal; and channel-6 traffic at
2431 to 2441 MHz at about this signal's level. A 36 MHz signal at 2420 MHz covers both.
The B200's default 16 receive frames also overflowed at 50 MS/s. `num_recv_frames=256` fixed
that for later captures.

Open: an on-air 80 MS/s comparison needs a quieter band or less attenuation (at least 30 dB).
Radio-off timing and memory at 80 MS/s are covered by the paces above.
