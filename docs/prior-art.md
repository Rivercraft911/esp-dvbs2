# Prior art

Most of the radio tricks in this transceiver were found by other people first. Their findings
are cited here with links to the exact revisions I read; none of their code is in this
repository. Independent host receivers run as separate, locally installed tools.

The reference was checked against separate transmitter harnesses using
[GNU Radio gr-dtv at aee9fd3](https://github.com/gnuradio/gnuradio/tree/aee9fd3f79389c4282a98e8d62c8405c73fd91df/gr-dtv)
and the LeanSDR revision below. Their code stays outside this repository. Independent
stage digests, numerical symbol comparisons and file-loopback evidence are documented in
[the APSK results](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-apsk-decode/README.md).

| Project | Chip | What it showed | Licence |
|---|---|---|---|
| [ESP32-DATV](https://github.com/SP8ESA/ESP32-DATV/tree/cd6246b62861ff2e20dfa91af51e7fab24804cd0) (SP8ESA) | C3 | DVB-S and DVB-S2 on 2300 to 2450 MHz up to 1 Msym/s. The CPU rewrites one held DAC word every 20 cycles (8 MS/s); a PC does the coding and sends symbols over USB. | [PolyForm Noncommercial 1.0.0](https://github.com/SP8ESA/ESP32-DATV/blob/cd6246b62861ff2e20dfa91af51e7fab24804cd0/LICENSE) |
| [ESP32-DATV-SA](https://github.com/barisdinc/ESP32-DATV-SA/tree/970b5611c909cb7741bfe9274b659953ddf97070) | C3 | A stand-alone fork that encodes beacon frames on the chip. | [PolyForm Noncommercial 1.0.0](https://github.com/barisdinc/ESP32-DATV-SA/blob/970b5611c909cb7741bfe9274b659953ddf97070/LICENSE) |
| [ESP-SDR](https://github.com/ESPARGOS/esp-sdr/tree/f86da81807b7f6e850742f6b789d9468c190ad41) (ESPARGOS) | most ESP32s | Raw I/Q capture from the receiver through an undocumented debug path, up to 80 MS/s. The S3 receiver here captures through the same engine. | [GPL-3.0-or-later](https://github.com/ESPARGOS/esp-sdr/blob/f86da81807b7f6e850742f6b789d9468c190ad41/LICENSE) |
| [esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr/tree/3214c16a7c9f136414cc5ae9d245ea290c98a6ce) | S3 | Arbitrary I/Q transmit through the dump/DAC engine at 40 or 80 MS/s, gapless streaming from two banks, 20 Mbaud QAM at about 31.5 dB MER S3 to S3 through 30 dB. Register notes for the S3, including the dump engine the receiver here relies on. | [GPL-3.0-or-later](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/LICENSE) |
| [eSpDR](https://github.com/h0m3us3r/eSpDR/tree/f279bf823eee41796dfd1ac21f13e1ed9b418c82) | S3 | Receiver; [issue 3](https://github.com/h0m3us3r/eSpDR/issues/3) documents the DAC channel and the PLL tuning register. | [0BSD](https://github.com/h0m3us3r/eSpDR/blob/f279bf823eee41796dfd1ac21f13e1ed9b418c82/LICENSE) |
| [esp32-sdr-trx](https://github.com/jochenhammes/esp32-sdr-trx/tree/e1aebbf2ef508c7199a9b6c6c277b57fe0b51aa9) (DA2JH) | S3 | Receiver for SDR++, and FM/SSB voice on 13 cm by steering the test tone 40,000 times per second. | [0BSD](https://github.com/jochenhammes/esp32-sdr-trx/blob/e1aebbf2ef508c7199a9b6c6c277b57fe0b51aa9/LICENSE) |
| [ESP32SDR](https://github.com/lozaning/ESP32SDR/tree/dbff7bb8ca8470e92e18de9cfd3f81aabe55f6f1) | S3 | Tone keying (OOK/FSK); reported that DAC replay did not radiate, which later work explains as a setup issue. | none stated |
| [LeanSDR / LeanDVB](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3) (pabr) | host | Independent DVB-S2 IQ receiver. `host/decode.py` runs a separate local executable; its BBFRAME output supports GCS payload comparison. Receiver source stays outside this repository; the self-check supports stock debug output and extended status. Its short 3/4 LDPC table needs a one-line fix, below. | [GPL-3.0-or-later](https://github.com/pabr/leansdr/blob/84c59e1c7a1a79338d5722d63f28640cc9d350f3/LICENSE.txt) |

## What this project takes from them

- **Transmit path (esp-sdr-zephyr):** the S3's DAC engine loops a 16380-word SRAM bank at
  40 MS/s. Each TX word is one complex sample: I in bits 9:0, Q in bits 19:10, both signed
  10-bit. RX uses the opposite field order ([register notes, lines 1355-1365](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1355-L1365)).
  The CPU fills the idle bank and switches banks at a scheduled time. With 64 lead and 2048 tail
  samples of overlap, a new bank is due every (16380 − 64 − 2048) / 40 MS/s = 356.7 µs.
- **Bringing the transmitter up (esp-sdr-zephyr):** starting and then stopping the vendor CW
  test tone turns the whole transmit chain on with the PHY's calibrated settings and leaves the
  DAC on the data path ([esp_sdr_paced.c](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/lib/esp_sdr/src/esp_sdr_paced.c#L203-L232)).
  Forcing the front end without it passed only the I branch on this board
  ([results](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-radio/README.md)).
- **Tuning:** the S3's PLL locks from about 1.8 to 2.8 GHz (esp-sdr-zephyr), which covers the
  13 cm band. Conducted checks here decoded exact payloads from 2000 to 2420 MHz
  ([S-band](../results/2026-10-20-s-band/README.md), [lower edge](../results/2026-10-20-band-followup/README.md)).
- **Filter controls (esp-sdr-zephyr):** BBTOP block `0x67` registers 4/5 control the receive
  low-pass filter. Separate transmit pairs are 12/13 and 14/15, with codes in bits 5:0
  ([register notes, lines 1189-1251](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1189-L1251)).
  Its reported response within about 3 dB through +/-18 MHz in bandwidth mode 2 is an
  end-to-end measurement between two S3s, including both filters
  ([hardware quirks, lines 56-65](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/hardware-quirks.md#L56-L65)).
  One addition from this project: the CW bring-up resets PBUS(4,2), which the bandwidth call
  leaves alone, so the transmitter here restores it to 32 after setup and reads it back. Before
  that fix our E200 chirp measured about 20 MHz (-3 dB at ±10 MHz); after it, single B200
  captures decode exact payloads at 36 and 40 MHz
  ([wide filter](../results/2026-10-20-wide-filter/README.md)).
- **Receive path (ESPARGOS, esp-sdr-zephyr):** the receiver captures 16 MS/s continuously
  through the dump engine on a second S3, rotating the engine through three 64 KiB SRAM banks.
  It relies on esp-sdr-zephyr's register notes for the dump control register
  ([lines 196-246](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L196-L246)),
  the lane selectors ([312-332](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L312-L332)),
  bank ownership ([343-383](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L343-L383)),
  the receive word format with Q in bits 9:0 and I in bits 19:10
  ([1355-1365](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1355-L1365))
  and the vendor calls that force the receive chain on
  ([1377-1386](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1377-L1386)).
  Their notes also describe the circular capture: switching the bank select while the engine
  runs splits one stream into gapless units, checked with sentinel words, a ring that esp-sdr-zephyr
  credits to upstream ESP-SDR
  ([hardware quirks, lines 138-144](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/hardware-quirks.md#L138-L144)).
  This project adds the timer-driven bank handoff and the whole receiver after the samples
  ([receiving](how-it-works.md#receiving)).
- **Clock from carrier (esp-sdr-zephyr):** both boards' sample clocks differ by the same ratio
  as their carriers, and their QAM receiver derives one from the other
  ([hardware quirks, lines 91-99](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/hardware-quirks.md#L91-L99)).
  The demodulator here seeds its timing loop the same way; a float fit on 15 captures agreed
  within 0.04 ppm, and within 0.01 ppm on 13 of them
  ([demodulator](../results/2026-10-22-rx-demod/README.md#clock-against-carrier-offset)).
- **Capture without Wi-Fi (this project):** esp-sdr-zephyr's notes say the engine and the PHY
  calls need the Wi-Fi driver up
  ([line 1386](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1386)).
  Here, stopping Wi-Fi froze the dump at a constant until a register diff showed why: stopping
  Wi-Fi clears bit 1 of `SYSCON_WIFI_BB_CFG_REG`. The receiver holds the PHY through the
  Bluetooth modem bit, deinitializes Wi-Fi and sets that bit again
  ([receiving](how-it-works.md#receiving)).
- **Self capture:** one S3 measuring its own transmission is still untested. The
  [archived feasibility assessment](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/docs/wide-filter.md#can-the-s3-measure-itself)
  separates the controls found in ROM from a working spectrum self-test. The receiver above is a
  second board.
- **LeanDVB's short 3/4 table:** its `ldpc_sf_fec34` table lists 12 addresses in the first row
  but sets `ncols` to 3, which costs about 292 LDPC bit flips per clean frame. Our row matches
  the standard and gr-dtv; with `ncols` 12 the same samples decode with zero flips. Short-frame
  decodes here use that one-line local fix
  ([short frames](../results/2026-10-22-short-frames/README.md#leandvb-short-34-table-error)).

## MER, theirs and ours

MER (modulation error ratio) is the power of the ideal symbols over the mean power of the error
between each received symbol and its ideal point.

esp-sdr-zephyr reports MER ceilings of about 31.5 dB for 20 Mbaud single-carrier QAM at
80 MS/s, with or without attenuation, and about 31 dB for 16 MHz OFDM
([hardware quirks, lines 80-89](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/hardware-quirks.md#L80-L89)).
Its S3-to-S3 link app measured about 31.5 dB through a 30 dB attenuator
([apps/link, line 39](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/apps/link/README.rst#L39)).

The S3 receiver here measured a median pilot MER of 29.9 dB (lowest 26.8 dB) on 8PSK 8/9 at
5 Msym/s, S3 to S3 through coax and 30 dB at 2420 MHz, and 30.4 dB on QPSK 3/4 at 1 Msym/s
([results](../results/2026-10-23-rx-5m-margin/README.md)). The two are close but not the same
measurement. Ours comes from the DVB-S2 header and pilot symbols, after the demodulator removes
the transmitter's carrier leak, about 17 dB below the signal. That removal alone moves our figure
from about 16 dB to about 29 dB, and LeanDVB, which does not do it, reports 7 to 16 dB on the
same samples ([demodulator](../results/2026-10-22-rx-demod/README.md)).

## Our own earlier work

The DVB-S2 reference model, its tests and the table-driven pulse shaping come from the
[RP2350 I/Q benchmark at 88ab018](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/tree/88ab018e65a51805ff562842177a4d47b1b04b2e), where the same
chain ran at 16 Msym/s on an RP2350 driving a TI AFE7071 DAC.

That same-author snapshot supplies reference-model, test and shaping code, and
`reference/plotstyle.py` supplies the original figure style. It has no standalone licence file;
the code carried into esp-dvbs2 is published here by River Dowdy under
[Apache-2.0](../LICENSE), with the project notice in [NOTICE](../NOTICE).
This provenance statement does not relicense separately installed third-party tools.

The first S3 receiver, a finite one that collected 400 ms and then decoded two frames, is kept
at [archive/exp/s3-receive](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/exp/s3-receive).
The continuous receiver took its RF setup order and its sentinel search for the engine's write
edge from there ([finite receiver results](measurements.md#receiver-experiments)).
