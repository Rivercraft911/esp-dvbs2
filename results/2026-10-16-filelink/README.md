# File packets through LeanDVB, with frame erasures

**Host simulation:** an 11,000-byte file crossed the Python DVB-S2 encoder,
synthetic IQ and a separate LeanDVB process without a changed byte. All 10
combinations of two erased file BBFRAMEs recovered the original file. With
three erased frames, recovery rejected the incomplete file. No board, SDR or RF
was used. Erasures were injected **after** decoding, so this does not establish
RF outage reacquisition, receiver sensitivity or real-time throughput.

## Run

`runs.jsonl` records the 2026-10-16 run, Python/numpy versions, source hashes,
receiver executable hash and file digest. Source was the uncommitted work on
`exp/video-path` over `058387aca9f4fc6d9c487b08f9355e959dfd9f1b`; the environment
denied creating the worktree's Git index lock, so no new commit was available.
The hashes identify the exact tested source. Firmware is null because none ran.

```sh
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 tests/test_filelink.py \
  --loopback \
  --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  --out results/<new-date>-filelink/runs.jsonl
```

Receiver: upstream [LeanSDR 84c59e1c](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3)
with the existing local macOS/status patch described in
[the decode results](../2026-10-16-decode/README.md). Executable SHA-256:
`5482ab74d714959498cf2ad8f177b195194c81850c4e479890bb2a4d695dcfa0`.

## Signal and application

| Quantity | Value |
|---|---|
| Waveform | Simulated noiseless 8PSK 2/3, normal frames, pilots on, PL code 0 |
| Symbol/sample rates | 5 Msym/s / 10 MS/s, cf32 little-endian |
| Pulse | `host/iq.py`, RRC roll-off 0.35, default 12-symbol span |
| Application coding | 3 source + 2 repair packets, Cauchy Reed–Solomon over GF(256) |
| Packet size | 5314 shard bytes + 56 header/CRC bytes = one 5370-byte DATA FIELD |
| File | 11,000 pseudorandom bytes, seed 20261016 |
| Guard/file/guard | 4 / 5 / 8 physical frames |
| Failed BCH frames | 0 |
| Residual LDPC checks | 0 |
| File frames recovered before artificial erasures | 5 of 5, exact |
| Two-frame erasure patterns | All 10 pass, both BBHEADER-record and flat-GCS inputs |
| Three-frame erasure control | Rejected |

File SHA-256: `7f6b79a056aec2e470cf6d411bd9601f599818df72bcb92f176a7471829aad0e`.
The BBFRAMEs use user-private GCS SYNC=0xb9 and are encoded through the existing
`host/dvbs2.py` mode adapter, BCH, LDPC and physical-layer framing. The receiver
emits its native `--fd-bbf` records. Both the record parser and the concatenated
GCS packet scanner recover the same original bytes after erasures.

## Other host checks

The same command first runs the ordinary tests: exhaustive comparison of all
65,536 GF products with independent polynomial multiplication; a fixed parity
vector; all 65 any-k subsets across four small code configurations; boundary
codes with k+m=256; packet CRC, whole-file SHA, metadata, duplicate, reorder,
truncation, empty-file and missing-block cases; actual GCS mode-adapter output
for short 1/4 and normal 1/2, 2/3, 9/10; one-packet and four-packet frame loss;
CLI success and failure without replacing existing output. Ordinary tests are
registered in CI; independent LeanDVB remains an optional local test.

The default 16+4 code's four-erasure guarantee is per block, not per file.
Its full-block 7.6619 Mb/s goodput at 5 Msym/s 8PSK 2/3 and 88.776 ms block time
are **derived**, not measured throughput. See [docs/file-link.md](../../docs/file-link.md)
for equations, sender/receiver candidates, integration gaps and coordinator checks.
