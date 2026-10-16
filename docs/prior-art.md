# Prior art

The radio tricks this project relies on were found by others. Their findings are cited here;
none of their code is used, since several licences do not allow reuse in an Apache-2.0 project.
Facts below were read from each project's source and documents on 2026-10-16.

| Project | Chip | What it showed | Licence |
|---|---|---|---|
| [ESP32-DATV](https://github.com/SP8ESA/ESP32-DATV) (SP8ESA) | C3 | DVB-S and DVB-S2 on 2300–2450 MHz up to 1 Msym/s. The CPU rewrites one held DAC word every 20 cycles (8 MS/s); a PC does the coding and sends symbols over USB. | PolyForm Noncommercial 1.0.0 |
| [ESP32-DATV-SA](https://github.com/barisdinc/ESP32-DATV-SA) | C3 | A stand-alone fork that encodes beacon frames on the chip. | PolyForm Noncommercial 1.0.0 |
| [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) (ESPARGOS) | most ESP32s | Raw I/Q capture from the receiver through an undocumented debug path, up to 80 MS/s. | GPL-3.0-or-later |
| [esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr) | S3 | Arbitrary I/Q transmit through the dump/DAC engine at 40 or 80 MS/s, gapless streaming from two banks, 20 Mbaud QAM at about 31.5 dB MER S3 to S3 through 30 dB. Register notes for the S3. | GPL-3.0-or-later |
| [eSpDR](https://github.com/h0m3us3r/eSpDR) | S3 | Receiver; [issue 3](https://github.com/h0m3us3r/eSpDR/issues/3) documents the DAC channel and the PLL tuning register. | 0BSD |
| [esp32-sdr-trx](https://github.com/jochenhammes/esp32-sdr-trx) (DA2JH) | S3 | Receiver for SDR++, and FM/SSB voice on 13 cm by steering the test tone 40,000 times per second. | 0BSD |
| [ESP32SDR](https://github.com/lozaning/ESP32SDR) | S3 | Tone keying (OOK/FSK); reported that DAC replay did not radiate, which later work explains as a setup issue. | none stated |

## What this project takes from them

- **Transmit path (esp-sdr-zephyr):** the S3's DAC engine loops a 16380-word SRAM bank at
  40 MS/s. Each word is one complex sample: I in bits 19:10, Q in bits 9:0, both signed 10-bit.
  The CPU fills the idle bank and switches banks at a scheduled time. With 64 lead and 2048 tail
  samples of overlap, a new bank is due every (16380 − 64 − 2048) / 40 MS/s = 356.7 µs.
- **Tuning:** the S3's PLL locks from about 1.8 to 2.8 GHz (esp-sdr-zephyr), which covers the
  13 cm band.
- **Quality:** the measured 31.5 dB MER is far above what QPSK and 8PSK need, so the radio is not
  expected to limit this project's modulation.

## Our own earlier work

The DVB-S2 reference model, its tests and the table-driven pulse shaping come from the
[RP2350 I/Q benchmark](https://github.com/Rivercraft911/RP2350_IQ_Benchmark), where the same
chain ran at 16 Msym/s on an RP2350 driving a TI AFE7071 DAC.
