# Correctness fixtures

`crosscheck_digests.json` contains independent gr-dtv/LeanSDR stage digests, not output
from our reference model. Its `_about` field names the source revisions and input.

`waveform_crcs.json` pins 65 dumps. The original 58 entries are unchanged from
`88f5a64`. Seven additional 1 Msym/s QPSK entries were generated from that original
source during cleanup, before comparing the new source. `moon_stream_crcs.json` pins
all eight QPSK 33m333 modes against the same original source. Its existing MODCOD 4/11
CRCs are unchanged; unsupported experimental 8PSK entries were removed.

Keys are `MODCOD/samples-per-symbol/DAC-MHz/frames`. `12` represents 12/5 samples
per symbol for 33m333. All dumps use pilots, peak 128 and stored callsign N0CALL.
Decimation is 10 for 1 Msym/s QPSK, 2 for 5 Msym/s QPSK, and 1 otherwise.
CRCs cover the emitted little-endian int16 I/Q bytes. They include no receiver flush.

Together the 73 fixtures cover every supported modulation/rate combination, plus
several historical frame-count regressions. The stream test also compares every
filled word against the portable encoder and shaper; fractional samples are checked
against independent Python convolution. These are software correctness checks,
not board timing measurements. Do not regenerate fixtures to accept a changed waveform.

`tone_crcs.json` pins the retained test tones against `88f5a64`: 4,097 packed DAC words,
starting at sample 2^32+17. Callback splits must preserve that sequence. Removed
PRBS, sweep and gap probes are intentionally rejected.
