# First SPI input run: receive works, integrated timing and heap fail

Coordinator-supplied measurements of **cb1805b**, 2026-10-17 09:25 UTC.
Bare DevKit, SPI3-to-SPI2 pad loopback, radio off. No agent hardware access.
`runs.jsonl` is copied byte for byte from the coordinator's saved records.
`gcs.decode.json` is the coordinator's independent LeanDVB result.
The original `notes/spi-in-board.log` SHA-256 is
`02adeee13969855fec69976bdea9634e5f2b83088f646d1591d06bf77c714230`;
`runs.jsonl` SHA-256 is
`89c4e5f2e57c96489813a533675d5d9278e9ece77de42dd79030823b5202d49d`.

Measured:

- GCS receive and independent pattern verification: 43.174 Mb/s at 46.6 offered,
  64.771 Mb/s at 87 offered; zero bad accepted payload. Neither meets the 98% gate.
- Faults 1/2/3: CRC/short/long counters 47 each. Long injection also caused two
  overruns and two restarts. Ignoring READY caused 11 overruns/restarts and 440
  sequence losses. Obeying it during consumer holds caused zero loss, with 470 waits.
- Isolated 1,316 B copy windows: maxima 2,301–3,048 cycles, including interruptions.
- GCS 16-frame QPSK dump: CRC 2507118847; 14 contiguous exact decoded frames,
  zero FEC errors; active encode mean 282,738, live maximum 289,707 cycles/frame.
- Paced QPSK 9/10 at 26.67 Msym/s: 240,201 underruns; active encode mean 755,448,
  live maximum 3,887,465 cycles/frame; only seven frames encoded. Only 39 messages
  accepted. Its planned 3,235,200,000 samples at 80 MS/s include station IDs and take
  40.44 s, so total accepted input averages only 0.01015 Mb/s.
- Wi-Fi probe with loopback: `ESP_ERR_NO_MEM`, free-before 22,083 B, largest block
  10,752 B, minimum 8,335 B. These are failed results, not a Wi-Fi reserve.

The scheduling diagnosis is derived from firmware: the low-priority sender is
pinned to core 1, whose priority-23 timed worker never yields during its schedule.
The untimed USB dump allows it to run. This explains why dump correctness does not
establish paced input throughput. The revised branch needs new silicon results;
none of its timing or heap predictions are recorded here as measurements.
