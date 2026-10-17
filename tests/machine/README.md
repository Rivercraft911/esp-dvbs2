# Sign-window queue kernels

`python3 tests/test_machine.py` checks 2,880 guarded packed prepares and 17,280
sample slices against independent full-axis integer convolution. The standalone
16-frame MODCOD 11 dump retains wild-a f704aa0's CRC 2575466360.
`tests/test_rational.py --receiver /path/to/leandvb` checks the actual transmitter
queue, counter wraps and 200 frame/bank seams. Its live startup uses different
padding from the standalone kernel, giving CRC 4118466753 for MODCOD 11 and
1616153472 for MODCOD 4. Both decode to 14 contiguous frames without corrections.
Clocks and heap sizes in native tests are synthetic.

Build this app with ESP-IDF v6.1 and run its merged flash in ESP32-S3 QEMU as
in [shaper_pie](../shaper_pie/README.md). `QEMU-END` and `machine_end` with zero
errors are required. Pass the serial log to `tests/test_machine.py --qemu log`
to check every table/history/sample hash against Python. QEMU tests instruction
semantics, never silicon timing. `MACHINE_TRACE=ON` selects short prepare/render
calls for issue-model traces. No QEMU address compensation is needed here.

The live mapper writes sign histories directly into 54 guarded pages of 128
blocks. It has no packed-frame ring or producer window. The standalone packed
prepare remains only as a kernel oracle and is discarded from the live image.
The 24 KiB rendering table and 4 KiB sign LUT stay internal.
[Gate and layout](../../docs/moonshot.md).
