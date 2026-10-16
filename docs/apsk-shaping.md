# APSK shaping prototypes

The compact single-symbol table is the first candidate to measure: 10 KiB for 16APSK,
18 KiB for 32APSK, including unit-energy QPSK points for headers and pilots. At 8 samples/symbol,
the steady loop models at 4.94 cycles/sample. Pair tables model at 4.71 cycles/sample but need
32.6 KiB for 16APSK or 129.1 KiB for 32APSK, before the QPSK fallback table.
These are host and pipeline models, not board timings.

With a 9-symbol RRC, roll-off 0.20, peak setting 128 DAC counts, rate 3/4 and 262,144 random
symbols, the host model gives about 38.3 dB modulation error ratio (MER) and +0.23 dB continuous
spectrum mask margin at 8 samples/symbol. The mask is EN 302 307-1 V1.4.1 Annex A, table A.1,
pp37-38, with an ideal DAC hold. The -40 dB limit beyond point S to Nyquist is an explicit
extension. Analog response, DAC images, discrete spurs and radio nonlinearity are not included.
The finite-record mask estimate is not a compliance result.

The 40 MS/s DAC would make 5 Msym/s at 8 samples/symbol. Its modelled steady fill consumes
95% of a core with the compact table, 90% with pairs, including duplicate bank overlap but
excluding call setup, partial symbols, symbol ingress, interrupts and DAC contention. This is
a candidate for measurement with little margin. The quality-preserving 4-sample kernels miss
the 40 MS/s bank deadline. None of these kernels fits 80 MS/s. The bank dimensions and packed
I/Q fields come from `firmware/components/radio_s3/include/radio_s3.h:7-24`; no radio integration
was changed. The measured compression near amplitude 128 remains a separate limitation
(`results/2026-10-16-radio/README.md:89-104`).

`host/shaper_apsk.py` derives every standard constellation radius and label from clauses
5.4.3/5.4.4, figures 11/12 and tables 9/10, pp28-30. `firmware/main/shaper_apsk.c` builds the
same tables. `shaper_apsk_pie.S` rotates row pointers and packs the previous result while
loading the next. Both whole-count and three-fractional-bit tables use four bytes per table
sample. Whole-count tables are faster but fail this host spectrum model at peak 128.

The single kernel accepts APSK and QPSK codes. Pair kernels accept data only, including
quarter-turn PL scrambling. A future caller must use the single kernel throughout a header
or pilot block and until eight data symbols have passed. The two methods round differently;
a transition needs a waveform and receiver check. They are not wired into `dvb`.

Run the host checks and report with a Python that has numpy and scipy:

```sh
python3 tests/test_shaper_apsk.py
python3 host/shaper_apsk.py --count 262144 --out notes/apsk-signal.jsonl
python3 host/shaper_apsk.py --all-rates --out notes/apsk-all-rates.jsonl
```

Create the isolated correctness app, build it and merge its flash image without flashing:

```sh
python3 tests/test_shaper_apsk.py --qemu-project notes/apsk-qemu
source ~/.espressif/tools/activate_idf_v6.1.sh
IDF_COMPONENT_MANAGER=0 idf.py -C notes/apsk-qemu build
cd notes/apsk-qemu/build
python -m esptool --chip esp32s3 merge-bin -o flash.bin --fill-flash-size 4MB @flash_args
```

Run `flash.bin` in Espressif QEMU `esp_develop_9.2.2`, machine `esp32s3`, with UART0 captured
to a file. The expected terminator is `QEMU-END checks1760 failures0`. The app compares 422,400
complex output words with C, checks all table hashes against Python, and checks output guards.
It includes all 11 radius settings, both sample rates, both precisions, and zero-length calls.
QEMU verifies instruction semantics, not cycle timing.

Disassemble that app, then model the actual loops:

```sh
xtensa-esp32s3-elf-objdump -d apsk_qemu.elf > apsk.dis
python3 /path/to/worktree/host/shaper_apsk_cycles.py apsk.dis
```

The cycle script uses the fec-limit pipeline method: ESP32-S3 TRM v1.8 section 1.7.1 pp65-66,
table 1.7-2 pp66-73 and section 1.7.3 p74. It rejects unknown instructions. It models single
issue, load-use interlocks and taken branches, with zero memory stalls. Its 12 loop estimates
match the copied fec-limit model exactly. PIE memory timing and radio contention need a board.
Long derivations and local run records belong in ignored `notes/apsk-shape.md` and
`notes/apsk-*.jsonl`, not in measured `results/` directories.
