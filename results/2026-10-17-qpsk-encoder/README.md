# QPSK encoder, offline

All 32 new QPSK configurations pass host stream checks and LeanDVB loopback:
MODCODs 4–11 at 5m, 10m, 10m80 and 20m. Each recovers 38 contiguous bit-exact
payload frames from a 40-frame dump, with zero LDPC flips, BCH corrections or
failures, and payload bit errors. The five published dump CRCs are unchanged.
No board, serial port, Wi-Fi or radio was accessed.

Firmware source: `56a9d5c` (code commits `51dc5b9`, `0923854`, with main
`32ea843` merged). The linked ELF and instruction-trace SHA-256 hashes are in
`runs.jsonl`. The compiler is ESP-IDF v6.1's Xtensa GCC 15.2.0. Host tests use
Python 3.12, numpy and scipy. LeanDVB is the supplied local executable at
`/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb`.

The instruction model projects 151k–186k cycles/frame including estimated
payload generation, or 189k–233k with a 25% sensitivity allowance. QPSK 9/10
projects 181k/227k respectively. **These are not board timings or bounds.**
Interrupts, cache misses and SRAM contention are excluded. QEMU CCOUNT is not
used as performance evidence.

The encoder QEMU harness passes 140 reference CRCs, 14 PIE checks (now including
packed QPSK) and 448 guarded packed frames. The shaper harness passes its 324
table/kernel checks, 3,360 sample-glue cases, 39,170 pack/unpack cases and 864
word-copy cases. Firmware and both harnesses build with IDF v6.1.

`runs.jsonl` contains 114 offline records: provenance, instruction counts/model
cycles, linked sizes, requested memory, 45 stream configurations, 32 loopbacks,
four default-decimation CRC predictions, QEMU checks and build status. Host
heap sizes/clocks are synthetic; `ram_requested` sizes come from the S3 ABI.
Requested heap totals exclude allocator headers and task control blocks.

Reproduce the complete stream and optional independent receiver checks:

```sh
python3 tests/test_dvbs2_c.py
python3 tests/test_encoder_tables.py
python3 tests/test_tx_command.py
python3 tests/test_stream.py notes/qpsk-stream --receiver /path/to/leandvb
```

QEMU commands are in `tests/encoder_pie/README.md` and
`tests/shaper_pie/README.md`. Research trace tooling and logs remain in
`notes/qpsk-model`, outside version control. The cycle/RAM derivation and
ordered radio-off coordinator commands, with pass criteria, are in
[docs/qpsk-encoder.md](../../docs/qpsk-encoder.md). Silicon timing, allocation
success and board-dump decoding remain outstanding.
