# Wideband RF figures

The README's 36 and 40 MHz figures use the original B200 recordings from the
[wide-filter tests](../2026-10-20-wide-filter/README.md). Each is one continuous
41.94304 ms capture at 50 MS/s, centered on 2420 MHz. The fixture was a XIAO S3
through 40 dB attenuation into the B200, with prepared, repeated payloads.
The stock receiver recovered 47 exact frames at 8PSK 9/10 and 40 at QPSK 9/10.

The NPZ files keep averaged power, rather than another copy of the raw IQ.
The calculation uses nonoverlapping 4,096-sample symmetric Hann windows and divides
power by sample rate times window energy. It then averages two time windows and
four frequency bins in linear power. The upper trace averages all 256 rows;
the waterfall shows those rows in order, with time increasing upward.
Its color scale is fixed at −130 to −85 dBFS/Hz in both figures. These are receiver
sample levels, without an absolute RF power calibration. No frequency stitching,
DC removal or equalization is applied.

Regenerate the figures from the committed summaries:

```sh
python3 host/plots.py --rf-only
```

With the original capture folders available, regenerate the summaries too:

```sh
python3 host/plots.py --rf-only --rf-iq-dir /path/to/captures
```

That directory must contain `qpsk910-33m333/capture.cf32` and
`8psk910-26m667/capture.cf32`. `runs.jsonl` pins both original IQ hashes and
the compact summaries. The script verifies them against the original run records.
