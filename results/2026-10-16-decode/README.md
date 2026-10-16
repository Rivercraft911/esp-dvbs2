# Independent DVB-S2 file loopback

All 40 host loopbacks recovered the complete application payload bit for bit. No hardware
was accessed. These results establish encoder/waveform interoperability with LeanDVB;
they do not measure RF quality, receiver sensitivity, or transmitter throughput.

Base commit `a5deb1b` on `exp/decode`, with the new host tools/tests uncommitted. Native arm64
macOS, Python 3.13.2, numpy 2.2.5, scipy 1.17.1, Apple clang 17.0.0. Independent receiver:
[pabr/leansdr](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3),
revision `84c59e1c7a1a79338d5722d63f28640cc9d350f3`, built in ignored `notes/`. Local patch:
macOS portability, per-attempt FEC statistics, and a full-frame acquisition search.

| Input | SHA-256 |
|---|---|
| `host/dvbs2.py` | `5cb367b38a22025c58e085d6e48b11ba57cf6e6bf38d8a7b6fc87f586930468f` |
| `host/iq.py` | `6748e5afa061323e39fcc8848d64c0799255193d00a0d3adce9e9df8ca1a8582` |
| `host/decode.py` | `2f2228fce579fabe3ed61800f4cae1eb8b8f87fdc2e86e0fc8af964c6a3ad8b1` |
| `tests/test_decode.py` | `fe3f153d82ee63da265cb440fc31134aaf1574b6ad3a7567dfbf30b13a658409` |
| `notes/leandvb-macos.patch` | `254fa0166c632833479c40095d6751558620341583e6ce157f4725fd609f4f1e` |
| receiver executable | `5482ab74d714959498cf2ad8f177b195194c81850c4e479890bb2a4d695dcfa0` |

Run, after building the local receiver:

```sh
python3 tests/test_decode.py --loopback --out results/<new-date>-decode/runs.jsonl
```

Each mode has eight cases: symbol rates 1 and 5 Msym/s, RRC roll-offs 0.20 and 0.35, clean
and impaired. Sample rates are 4 and 10 MS/s respectively. Impaired cases add circular
complex AWGN at 14 dB sample SNR, carrier offset `+0.01 Rs` (+10 or +50 kHz), and capture
clock error +50 ppm. Seed: 20261016. The matched-filter Es/N0 is approximately 20.02 dB
at 4 samples/symbol and 17.01 dB at 2 samples/symbol. Signals have normal frames, pilots
enabled, PL scrambling code 0, and 12-symbol RRC pulses.

| MODCOD | Unique payload per case | Cases passed | Valid BBFRAMEs | BCH failures |
|---|---:|---:|---:|---:|
| QPSK 1/2 (4) | 8,169 bytes | 8/8 | 72 | 0 |
| QPSK 2/3 (6) | 10,877 bytes | 8/8 | 72 | 0 |
| QPSK 3/4 (7) | 12,219 bytes | 8/8 | 72 | 0 |
| 8PSK 2/3 (13) | 10,877 bytes | 8/8 | 60 | 0 |
| 8PSK 5/6 (15) | 13,577 bytes | 8/8 | 60 | 2 |

The 3,566,016 compared payload bits all match. Each signal contains four training frames,
three unique application frames (last partial), and four tail frames. The complete application
file must appear contiguously in the receiver output. Endpoint guard loss is allowed. Clean
cases additionally require zero BCH failures and zero unsatisfied LDPC checks after decoding.

Both BCH failures occurred on guard frames in impaired 8PSK 5/6 at 5 Msym/s. The rejected
attempts falsely indicated MODCOD 14. Full application files still matched, including their
third partial frame. `modcods` reports BCH-accepted frames; `attempted_modcods` also retains
those incorrect acquisitions. Across all cases: 338 FEC attempts, 336 valid BBFRAMEs,
5,858 decoder bit flips. These flips and parity-check counts are not channel bit-error rates.

The same test command also passed three supplemental checks: 8PSK 5/6 with an 80 MS/s DAC
hold, 18 dB sample SNR, -50 kHz carrier offset and -50 ppm clock error; a capture beginning
0.73 frames into the waveform; and QPSK 2/3 TS recovery (742 packets, 200 central packets /
37,600 bytes compared exactly, all transport-error flags clear). An altered expected file
fails comparison. These checks are printed by the test command; the 40 matrix records are
in `runs.jsonl`.

Long build/capture instructions and the receiver patch are retained in ignored `notes/`.
