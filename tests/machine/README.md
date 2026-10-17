# Queue kernels

`python3 tests/test_machine.py` checks 2,880 guarded prepares and 17,280
sample slices against independent integer convolution and retained waveform
CRC 3130880472. `tests/test_rational.py --receiver /path/to/leandvb` checks the
actual transmitter queue, including counter wraps and 200 frame/bank seams.
Clocks and heap sizes in native tests are synthetic.

Build this app with ESP-IDF v6.1 and run its merged flash in ESP32-S3 QEMU.
`QEMU-END` plus `machine_end` with zero errors is required. Pass the serial
log to `tests/test_machine.py --qemu log` to check every table/row/sample hash
against Python. QEMU tests instruction semantics, never silicon timing.

The live image links only `machine.c` and `machine_pie.S`, not the older
split kernels. Hardware admission is in [moonshot.md](../../docs/moonshot.md).
