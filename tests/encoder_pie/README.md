# Encoder PIE checks in QEMU

This app runs the actual `fecbench pie` command, then checks 384 complete packed
frames: six 8PSK MODCODs, pilots on/off, all 16 output alignments, both the direct
mapper with resident byte BCH (t = 10/12) and sparse mapper with nibble BCH,
and guarded output buffers. Its oracle is the portable C
encoder, whose stages are checked against Python by `tests/test_dvbs2_c.py`.
No radio or peripheral is initialized. QEMU establishes correctness, not timing.
QEMU has no PSRAM, so `bench.c` redirects its allocation capability to internal
RAM. The command's printed PSRAM metadata does not describe these emulated buffers.

From the repository root, after activating ESP-IDF v6.1:

```sh
IDF_COMPONENT_MANAGER=0 idf.py -C tests/encoder_pie build
cd tests/encoder_pie/build
python -m esptool --chip esp32s3 merge-bin -o flash.bin --fill-flash-size 4MB @flash_args
cd ../../..
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/encoder_pie/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/encoder-qemu.log -monitor none
```

Stop QEMU after `QEMU-END`. On macOS, set `DYLD_LIBRARY_PATH` to the SDL2 library
directory if the QEMU binary requires it. Filter the serial log before using the
JSONL checker:

```sh
python3 - <<'PY'
from pathlib import Path
text = Path('notes/encoder-qemu.log').read_text()
assert 'QEMU-END' in text and 'packed PIE: 384 cases' in text
Path('notes/encoder-qemu.jsonl').write_text(
    '\n'.join(line[1:] for line in text.splitlines() if line.startswith('@{')) + '\n')
PY
python3 tests/test_fecbench.py notes/encoder-qemu.jsonl --correctness-only
```

Pass: all 140 reference CRCs, 14 PIE checks with zero mismatch, the 384-case
packed success line, and `QEMU-END` with no assertion or exception. The printed
CCOUNT values are never performance results.
