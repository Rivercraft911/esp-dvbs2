# Pack before PSRAM

Packing each ADC bank into private internal RAM before GDMA cuts the stored
record from 8 MB to 4 MB and removes the packing pass after capture. Vector
sentinel validation precedes all output writes. DMA reads the packed stage
while the CPU clears the old radio bank.

The full 2067 MHz bank operation fell from a maximum of 173,580 cycles in
raw capture c5daf5b to 126,719 cycles in packed capture 2684ee7, about 27%
fewer cycles. Both used 2M pairs, QPSK 1/2 at 1 Msym/s and gain index 48.
The packed pattern and quiet controls reached 125,088 and 126,818 cycles.
The 28,768-byte stage allocated after Wi-Fi setup; the largest available
internal block was 31,744 bytes.

Pattern, quiet, and paired 2420/2067 MHz RF captures passed CRC, raw snapshot,
stage/record guards, bank continuity, sentinel and DMA checks. Each RF record
yielded two exact consecutive 4,016-byte packets through host LeanDVB.
No packet decoding ran on the S3.

This establishes the finite 125 ms path. It does not qualify continuous
capture or every allowed late-switch geometry. RX_PACKED_CAPTURE defaults
to zero; seven checked raw hot-function opcode sequences remain identical
to c5daf5b. The standalone packing benchmark's 3.27-fold gain is a component
measurement, not the whole-bank speedup reported here.
