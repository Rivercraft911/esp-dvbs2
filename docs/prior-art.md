# Prior art

The radio tricks this project relies on were found by others. Their findings are cited here;
none of their code is incorporated into this repository, since several licences do not allow
reuse in an Apache-2.0 project. Independent receivers run as separate, locally installed tools.
Facts below were read from each project's source and documents on 2026-10-16.

The APSK host reference is also checked against separate transmitter harnesses using
[GNU Radio gr-dtv at aee9fd3](https://github.com/gnuradio/gnuradio/tree/aee9fd3f79389c4282a98e8d62c8405c73fd91df/gr-dtv)
and the LeanSDR revision below. Their code stays outside this repository. Independent
stage digests, numerical symbol comparisons and file-loopback evidence are documented in
[the APSK results](../results/2026-10-16-apsk-decode/README.md).

| Project | Chip | What it showed | Licence |
|---|---|---|---|
| [ESP32-DATV](https://github.com/SP8ESA/ESP32-DATV) (SP8ESA) | C3 | DVB-S and DVB-S2 on 2300–2450 MHz up to 1 Msym/s. The CPU rewrites one held DAC word every 20 cycles (8 MS/s); a PC does the coding and sends symbols over USB. | PolyForm Noncommercial 1.0.0 |
| [ESP32-DATV-SA](https://github.com/barisdinc/ESP32-DATV-SA) | C3 | A stand-alone fork that encodes beacon frames on the chip. | PolyForm Noncommercial 1.0.0 |
| [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) (ESPARGOS) | most ESP32s | Raw I/Q capture from the receiver through an undocumented debug path, up to 80 MS/s. | GPL-3.0-or-later |
| [esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr) | S3 | Arbitrary I/Q transmit through the dump/DAC engine at 40 or 80 MS/s, gapless streaming from two banks, 20 Mbaud QAM at about 31.5 dB MER S3 to S3 through 30 dB. Register notes for the S3. | GPL-3.0-or-later |
| [eSpDR](https://github.com/h0m3us3r/eSpDR) | S3 | Receiver; [issue 3](https://github.com/h0m3us3r/eSpDR/issues/3) documents the DAC channel and the PLL tuning register. | 0BSD |
| [esp32-sdr-trx](https://github.com/jochenhammes/esp32-sdr-trx) (DA2JH) | S3 | Receiver for SDR++, and FM/SSB voice on 13 cm by steering the test tone 40,000 times per second. | 0BSD |
| [ESP32SDR](https://github.com/lozaning/ESP32SDR) | S3 | Tone keying (OOK/FSK); reported that DAC replay did not radiate, which later work explains as a setup issue. | none stated |
| [LeanSDR / LeanDVB](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3) (pabr) | host | Independent DVB-S2 IQ receiver. `host/decode.py` runs a separate local executable; its BBFRAME output supports GCS payload comparison. Receiver source and local macOS/status patches stay in ignored `notes/`. | GPL-3.0-or-later |

## What this project takes from them

- **Transmit path (esp-sdr-zephyr):** the S3's DAC engine loops a 16380-word SRAM bank at
  40 MS/s. Each TX word is one complex sample: I in bits 9:0, Q in bits 19:10, both signed
  10-bit. RX uses the opposite field order ([register notes, lines 1354–1362](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1354-L1362)).
  The CPU fills the idle bank and switches banks at a scheduled time. With 64 lead and 2048 tail
  samples of overlap, a new bank is due every (16380 − 64 − 2048) / 40 MS/s = 356.7 µs.
- **Bringing the transmitter up (esp-sdr-zephyr):** starting and then stopping the vendor CW
  test tone turns the whole transmit chain on with the PHY's calibrated settings and leaves the
  DAC on the data path ([esp_sdr_paced.c](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/lib/esp_sdr/src/esp_sdr_paced.c#L203-L232)).
  Forcing the front end without it passed only the I branch on this board
  ([results](../results/2026-10-16-radio/README.md)).
- **Tuning:** the S3's PLL locks from about 1.8 to 2.8 GHz (esp-sdr-zephyr), which covers the
  13 cm band.
- **Filter controls (esp-sdr-zephyr):** BBTOP block `0x67` registers 4/5 control the receive
  low-pass filter. Separate transmit pairs are 12/13 and 14/15, with codes in bits 5:0
  ([register notes, lines 1189–1251](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1189-L1251)).
  Its reported response within about 3 dB through +/-18 MHz in bandwidth mode 2 is an
  end-to-end measurement between two S3s, including both filters
  ([hardware quirks, lines 56–65](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/hardware-quirks.md#L56-L65)).
  Our transmitter's cabled response remains unmeasured.
- **Quality:** the measured 31.5 dB MER is far above what QPSK and 8PSK need, so the radio is not
  expected to limit this project's modulation.

## Our own earlier work

The DVB-S2 reference model, its tests and the table-driven pulse shaping come from the
[RP2350 I/Q benchmark](https://github.com/Rivercraft911/RP2350_IQ_Benchmark), where the same
chain ran at 16 Msym/s on an RP2350 driving a TI AFE7071 DAC.
