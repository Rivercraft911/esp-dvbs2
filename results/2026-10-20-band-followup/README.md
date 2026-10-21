# Lower edge and soft FEC

At 2000 MHz, QPSK 3/4 at 5 Msym/s passed 30 seconds into a B200:
4,505 consecutive exact packets and 7.257 Mb/s of payload. The S3 used the
five-sixths LO through 40 dB attenuation. This independently checks the
transmitter frequency path alongside the separate S3 receive experiments.

A private host soft-FEC prototype passed 30 seconds of 2245 MHz 8PSK 2/3
at 5 Msym/s at B200 gain 40, then gain 20: 6,754 and 6,755 consecutive exact
packets, respectively, about 9.67 Mb/s. The gain-20 run matches the earlier
stock receiver settings; individual live runs do not isolate causality.
The saved-IQ controls do.

The prototype stops iterating when the BCH data and BBHEADER validate.
On the saved failure window, this cut whole-tail CPU from 302.5 to 95.1 ms
and the largest output gap from 219 to 21.5 ms. LDPC parity may still be
invalid at that point. Its unavailable syndrome metrics remain null.
The helper and frontend remain outside this repository.

An initial live attempt stopped after 375 exact packets because helper and
frontend diagnostics interleaved inside a status record. That attempt remains
failed. The revised helper emits each status with one atomic write. Both
passing live runs ended with clean helper EOF counts, no interior BCH failure,
no sample gaps and clean finite TX completion. No raw IQ was kept on success.
