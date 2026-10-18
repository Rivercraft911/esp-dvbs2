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

`psk8_26m667_crcs.json` pins 16 nominal frame lengths at 80/3 Msym/s, MODCOD
13/15/17, peak 128, pilots on and N0CALL. `psk8_26m667_frames.json` records the
zero-based data-frame indices before which standard dummy frames occur; repeated
indices mean a dummy pair. The five-symbol pulse and its three independent integer
roundings are checked against `tests/reference/psk8_26m667.py`.

The 50/46-page dump queues deliberately use dummy fallback: model-regenerated CRCs are 3,631,096,604 (15; pairs before 4/7/10/13) and 1,787,558,147 (17; pairs before 2..12); 13 stays 2,061,900,610 with no dummies.

These layouts match every sample of the saved 87b17a9 board dumps; the bb8fa84
MODCOD 17 dump matches the same bytes. The original data-only CRCs remain pinned
in `psk8_26m667_frames.json`. Native tests replay both data-only and dummy layouts
and compare every word with the independent Python encoder/convolution, without
board files. A dump's `frames` means sample duration, not its number of data frames.
The receiver can only recover complete data frames: 16/14/12 here, with a final
partial data frame at 15/17. Fill telemetry may also count dummies rendered beyond
the saved interval, so it must not be subtracted from the nominal frame count.

Together the 76 fixtures cover every supported modulation/rate combination, plus
several historical frame-count regressions. The stream test also compares every
filled word against the portable encoder and shaper; fractional samples are checked
against independent Python convolution. These are software correctness checks,
not board timing measurements. Do not regenerate fixtures to accept an unexplained waveform change.

`tone_crcs.json` pins the retained test tones against `88f5a64`: 4,097 packed DAC words,
starting at sample 2^32+17. Callback splits must preserve that sequence. Removed
PRBS, sweep and gap probes are intentionally rejected.
