# 200 ms S3 receive

Packed capture extends the record to 3.2M native pairs at 16 MS/s, using
6.4 MB of PSRAM. Every tested record passed bank continuity, DMA, sentinel,
stage/record guards, CRC, raw snapshot and cleanup checks.

Stock LeanDVB recovered at least four exact consecutive 4,016-byte packets
from 200 ms QPSK 1/2 records at these settings:

| MHz | Receive gain index |
|---|---|
| 2025, 2067, 2110 | 48 |
| 2200, 2245 | 36 |
| 2290, 2420, 2450 | 24 |

The 2420 MHz gain-24 closing reference also passed. Lower-gain attempts at
2200, 2245 and 2420 failed the four-packet gate and remain in the record.
2200 included an interior BCH failure; the initial stock PLS acquisition
and finite-input lookahead also limit recovered packet counts.

At 2000 MHz/gain48, stock recovered three packets and failed that gate.
The independent known-header/pilot receiver recovered all five whole packets
from the same record, with independent LDPC/BCH re-encoding and separate
payload comparison. A separate transmitter using the same firmware passed
30 seconds into a B200 at 2000 MHz, verifying the frequency path. Independent
controls recovered four exact packets at low-gain 2200 and five at 2245; the
first 2200 packet remains unresolved.

These are finite conducted captures through 30 dB attenuation. Decoding
and FEC ran on the host. They do not establish continuous reception,
on-chip demodulation/FEC, sensitivity or a reliable operational link.
Raw defaults remain unchanged.
