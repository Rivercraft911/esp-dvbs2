# Four-symbol n3 experiment, host and QEMU only

The model falls from rd-short's 60.7k to **43.1k cycles per steady bank**, still
above 42,804 at 80 MS/s. With the measured encoder it consumes **463.2k cycles
per frame across two cores**, against 399,492 available. No board, serial port,
DAC or radio was accessed. The candidate is isolated in `membench`.

Source/build revision `93c3713`, implementation `e23272e`, based on `ea0958d`.
ESP-IDF v6.1, QEMU `esp_develop_9.2.2_20260417`. `runs.jsonl` records source,
disassembly, QEMU-log and receiver hashes. The unchanged legacy encoder mean
262.1k cycles/frame is from `../2026-10-16-next/pace-nibble.jsonl`; it is not a
new measurement. Equations, memory accounting, limitations and coordinator
commands are in [docs/max-astra.md](../../docs/max-astra.md).

| Offline check | Result |
|---|---|
| All 27 `tests/test_*.py` scripts | pass, including 140 FEC reference CRCs |
| Existing QEMU shaper / sample / packed cases | 324 / 1,520 / 13,058 pass |
| New QEMU block cases | 24 pass; full tables and output hashes match Python |
| Firmware and QEMU IDF builds | pass without warnings |
| Full-bank instruction model | 46,426 instructions, 46,427 cycles, 16,380 samples |
| Main ELF internal capacity delta | +256 B, including alignment; no DRAM data/BSS increase |
| Main ELF flash text / rodata delta | +1,300 / +240 B |

Independent LeanDVB host loopbacks, each with sixteen input frames and twelve
contiguous recovered frames. Every run has zero payload bit errors, LDPC flips,
LDPC checks and BCH failures. CRC32 is over interleaved signed int16 I/Q samples.
MER is the known-symbol digital-sample model; no DAC hold or analog response.

| Pulse | MODCOD | IQ CRC32 | MER |
|---|---:|---:|---:|
| rd-short span 6 | 13 | 339035381 | 40.518 dB |
| block4 | 13 | 2269685677 | 40.572 dB |
| rd-short span 6 | 15 | 3535002931 | 40.511 dB |
| block4 | 15 | 631214921 | 40.575 dB |

The rd-short CRCs match `exp/rd-short` at `fdd68dc`. Regrouping changes rounding,
so block4 has new CRCs; the complete quantized mask remains unqualified. These
are synthetic waveforms, not firmware dumps or evidence of real-time throughput.

Reproduce after building `tests/shaper_pie`, using its README to execute QEMU:

```sh
python3 tests/test_maxrate.py notes/qemu.log
python3 tests/test_shaper.py notes/qemu.log
python3 tests/test_tx_samples.py notes/qemu.log
xtensa-esp32s3-elf-objdump -d --no-show-raw-insn \
  tests/shaper_pie/build/esp-idf/main/libmain.a > notes/block.dis
python3 host/maxrate.py --disassembly notes/block.dis --receiver /path/to/leandvb
```

This result rejects the evaluated kernel plus current encoder at 26.67 Msym/s.
It does not establish a universal hardware limit. The 13.333 Msym/s n3 candidate
has model headroom; the complete measured transmitter remains at 10 Msym/s.
