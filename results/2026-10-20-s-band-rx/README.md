# S-band receive points

Paired ESP32-S3s passed the two-packet test at 2025, 2067, 2110, 2245 and
2290 MHz: two consecutive exact 4,016-byte QPSK 1/2 packets in each 125 ms
record. The boards were connected through 30 dB of attenuation. Samples came
from the receiving S3 at 16 MS/s; LeanDVB and FEC ran on the host.

2025, 2067 and 2110 MHz used the five-sixths LO and receive gain index 48.
2110 initially failed at index 36 with two BCH failures. 2245 and 2290 MHz
used the direct LO and index 24. Gain indices are vendor settings, not dB.

The 2200 MHz test and repeat recovered one exact packet each, failing the
two-packet requirement; each had one total BCH failure. The opening
2420 MHz control passed. Both closing 2420 MHz controls recovered one exact
packet without BCH failures and failed the same requirement. These failures
remain in the record. Acquisition and decoder behavior need further work.

Offline controls recovered all 18 complete packets from six retained records
using an independent known-header/pilot receiver and external soft LDPC.
Independent BCH and LDPC re-encoding passed, then payloads were compared separately.
Matched-frontend soft FEC also repaired the 2110 and 2200 MHz misses. The closing
2420 MHz misses came from initial stock PLS acquisition. Stock lookahead prevented
attempting the last complete frame. Original qualification failures remain unchanged.

All eleven captures passed sample continuity, bank, DMA, sentinel, guard,
snapshot, CRC, LO-selector readback/restoration and finite-TX checks. Those
checks establish transport integrity, not analog signal quality or PLL lock.
This is finite reception at sampled frequencies, not continuous capture or
an embedded DVB-S2 receiver. Firmware and evidence hashes are in `runs.jsonl`.
