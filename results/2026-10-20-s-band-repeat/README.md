# 30-second downlink repeats

2200 and 2290 MHz each passed 30 seconds of changing 8PSK 2/3 data at
5 Msym/s through 40 dB of attenuation into a B200. Each recovered 6,755
consecutive exact packets: 36,274,350 payload bytes, 9.673 Mb/s and 99.947%
of normal-frame airtime. FEC corrections were used. There were no interior
BCH failures, sample gaps or TX underruns in either passing run.

The first 2290 MHz attempt was invalidated by a UHD overflow after 3,990
exact packets. Its bounded failure tail and hashes are retained. The retry
passed after target compilation stopped. These observations do not establish
what caused the overflow.

Both transmit windows ended cleanly after 45 seconds; the B200 only received.
Successful runs retained no raw IQ. These are individual 30-second results,
not a general reliability guarantee or native S3 receiver qualification.
The earlier 2245 MHz single-frame loss remains unresolved by the stock decoder.
