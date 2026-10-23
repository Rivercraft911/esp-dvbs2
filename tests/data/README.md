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
13/15/17, peak 128, pilots on and N0CALL. The five-symbol pulse and its three independent
integer roundings are checked against `tests/reference/psk8_26m667.py`.

A dump is not real time, so its bytes must not depend on how far the encoder got. Its fill
waits at every frame boundary until the whole next frame is published, and never inserts a
timing dummy (`lofty_mux.whole_frames`, set for `dvbdump` only; keyed runs and paces keep
real-time admission). The wait ends when the encoder stops or fails, or after 2 s without a
new page (no sender, or input that stops mid-frame). The dump then reports `ESP_FAIL`,
`whole_frame_timeout` and `padded_bytes`: it still sends the announced bytes, zero-padded, so
the host reads its final record. The pinned CRCs are therefore the data-only ones, computed from the
host model: 2,061,900,610 (13), 1,873,478,180 (15) and 233,370,528 (17), 16 complete data
frames each. `psk8_26m667_frames.json` repeats them as `data_only_crc32` with an empty
`dummy_before`.

Earlier dumps let the fill race the encoder, so their dummy count depended on timing
(pinned 0+8 for 15 and 2+21 for 17, later 0+3 to 2+14 on the same layout). Those layouts
stay as `forced_dummy_before`/`forced_crc32` (zero-based data-frame indices before which a
dummy occurs; repeated indices mean a pair; 3,631,096,604 for 15 with pairs before
4/7/10/13, 1,787,558,147 for 17 with pairs before 2..12). They matched every sample of the
87b17a9 board dumps. The native tests force not-ready admissions at those frames and
compare every word with the independent Python encoder and convolution, so the dummy
path stays covered without board files. A dump's `frames` means sample duration, not its
number of data frames.

Together the 76 fixtures cover every supported modulation/rate combination, plus
several historical frame-count regressions. The stream test also compares every
filled word against the portable encoder and shaper; fractional samples are checked
against independent Python convolution. These are software correctness checks,
not board timing measurements. Do not regenerate fixtures to accept an unexplained waveform change.

`tone_crcs.json` pins the retained test tones against `88f5a64`: 4,097 packed DAC words,
starting at sample 2^32+17. Callback splits must preserve that sequence. Removed
PRBS, sweep and gap probes are intentionally rejected.

`rx_short7.sc8` is 18,240 samples (1.14 ms) of a real S3 dump: QPSK 3/4 short frames from
the S3 transmitter at 2420 MHz through coax, RX gain 32, 2026-10-22 (notes capture
short7-gain32, samples 38,280 to 56,519, from 150 symbols before a PLHEADER). Int8 I then Q
per sample, floor(s10 / 4), exactly what rx_unpack produces. tests/test_rx_demod.py checks
acquisition, timing and the carrier estimate on it.

`rx_5m13.sc8` is 6,400 samples (2,000 symbols) of a real S3 dump at 5 Msym/s: 8PSK 2/3 normal
frames, TX peak 128, 2420 MHz, coax, RX gain 32, 2026-10-22 (notes capture
normal13-5m-peak128, samples 110,935 to 117,334, from 150 symbols before a PLHEADER through
the first pilot block), same format.

`file-payload.png` is the 237,171-byte PNG (SHA-256 cc9b3d12…4044828) sent in the file-transfer
run docs/measurements/ota/file-png-5m. It was docs/img/fill-history.png at the time; the copy
here keeps the bytes fixed when figures are regenerated. tests/test_file_ota.py expects 60 coded
frames from it.
