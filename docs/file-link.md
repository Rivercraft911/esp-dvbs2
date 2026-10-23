# Files over a one-way link

The USB and SPI senders' `--fec K:M` option adds application erasure coding to
the existing name/length/SHA-256 container. A block carries `K` source shards
and `M` repair shards. **Any K distinct shards recover the block**, without
ACKs. Each shard fills one GCS DATA FIELD: any M missing frames per block of
K+M are survivable, regardless of which frames they are. Longer bursts that
straddle blocks work only when neither block exceeds its budget.

```sh
python3 examples/usb-send/send.py send document.pdf --fec 16:4 --seconds 30
python3 examples/usb-send/send.py unpack decoded.gcs --fec -o recovered.pdf
# Or unpack received --fd-bbf records directly:
python3 examples/usb-send/send.py unpack decoded.bbf --bbf --fec -o recovered.pdf
```

`--fec` alone selects 16:4 and implies file mode, including stdin (`--name` sets
its basename). Explicit TS/raw-GCS mode conflicts with FEC. USB derives capacity
from `--modcod`; the [SPI sender](../examples/pi-sender/README.md) needs the same
`--modcod` as the S3. Start each file with a fresh USB/SPI input run and keep the
mode fixed. The firmware concatenates declared PV payload bytes, excluding PV
padding, then slices full data fields; dummy PLFRAMEs consume no input bytes.
Dropping bytes on USB/SPI or choosing the wrong MODCOD breaks shard alignment.

LeanDVB's normal stdout is TS-only. For GCS use `--fd-bbf`: ten-byte BBHEADER
followed by DFL/8 bytes. [`host/decode.py`](../host/decode.py) validates the header
and flattens these data fields. `unpack` accepts those bytes, or `--bbf` accepts
the records themselves. Damaged packet CRCs become erasures; resynchronization
scans magic plus CRC. Unordered frames and identical duplicates are accepted.
Mixed identities/parameters, contradictory duplicates, missing blocks, invalid
sequences and hash mismatches fail. Output is created only after both hashes
pass; paths in received names and existing output files are refused.

## Construction and wire format

[`host/filelink.py`](../host/filelink.py) implements **systematic Cauchy
Reed–Solomon over GF(256)**, the construction described by Plank and Xu,
[*Optimizing Cauchy Reed-Solomon Codes for Fault-Tolerant Storage Applications*,
NCA 2006](https://web.eecs.utk.edu/~jplank/plank/papers/NCA-2006.pdf)
([tutorial material](https://web.eecs.utk.edu/~jplank/plank/papers/CS-05-569.html)).
This is an independently implemented application format, not RFC 5510 or GSE.

The generator is `[I; P]`, with `P[r,c] = inv(r XOR (M+c))`, `r=0..M-1`,
`c=0..K-1`. Arithmetic uses polynomial 0x11d and primitive element 2. The two
sets are disjoint; every square Cauchy minor is invertible, hence any K rows of
the systematic generator are invertible. Each byte position is coded separately.
Source shards precede repair shards; the last source block is zero padded.
Require K,M >= 1, K+M <= 256. Sequence space is limited to 2^32 packets.

Each packet is exactly `(Kbch-80)/8` bytes for the selected normal-frame MODCOD:

| Field | Bytes | Meaning (integers big-endian) |
|---|---:|---|
| Magic/version | 4 | ASCII `PFL1` |
| Identity | 32 | SHA-256 of the complete unpadded inner container |
| Length | 8 | Inner container length |
| Sequence | 4 | `block*(K+M) + shard_index` |
| K, M | 1 each | Source and repair counts |
| Shard size | 2 | `(Kbch-80)/8 - 56` |
| Shard | variable | Source or repair bytes |
| CRC-32 | 4 | `zlib.crc32` of header and shard |

The inner container remains `PVFILE1\n`, little-endian uint16 UTF-8 basename
length, uint64 original file length, 32 SHA-256 bytes of the original file,
basename, file bytes. Repeating the outer identity/length/parameters in every
packet protects the manifest against frame-zero loss. Eight zero-filled data
fields precede the coded packets for acquisition; four follow for drain.
CRC and SHA-256 provide integrity, not sender authentication.

## Cost

Let C be data-field bytes and S=C-56. Each block carries K*S useful container
bytes in (K+M)*C transmitted data bytes. Ignoring tail padding and guards:
`expansion = (K+M)*C/(K*S) - 1`; goodput is `K*S/((K+M)*C)` times GCS capacity.

At MODCOD 13 (8PSK 2/3), C=5,370 and S=5,314:

| K:M | Loss budget per block | Repair/source | Total byte expansion |
|---|---:|---:|---:|
| 16:1 | 1/17 (5.9%) | 6.25% | 7.37% |
| 16:2 | 2/18 (11.1%) | 12.5% | 13.69% |
| 16:4 | 4/20 (20%) | 25% | 26.32% |
| 16:8 | 8/24 (33.3%) | 50% | 51.58% |

For 16:4, useful throughput is 79.17% of GCS capacity, about 7.66 Mb/s at
5 Msym/s before USB/SPI limits. QPSK 1/2 has C=4,016, S=3,960 and 26.77%
expansion. For an L-byte file and N-byte basename, the exact data-field cost is
`C * (12 + (K+M)*ceil((L+50+N)/(K*S)))`. Small files pay a full final block and
the 12 guard fields. DVB-S2's own BCH/LDPC, headers, pilots, dummies and station
ID add their existing airtime costs. The loss percentage is a **per-block
bound**, not a promised success rate for an unbounded random-loss channel.

## Proof without RF

```sh
python3 tests/test_filelink.py
RX=/path/to/leandvb
python3 tests/test_filelink.py --receiver "$RX"
```

Tests cover independent polynomial arithmetic, a fixed Cauchy vector, every
three-loss choice for 4:3, the 256-shard boundary, all 14 MODCOD capacities,
USB/SPI CLIs with fake devices, filenames, hashes and failed-output handling.
The optional receiver proof generates Python-model IQ and runs a separate
LeanDVB process for QPSK 1/2 and 8PSK 2/3. It destroys coded bits in four frames
per block, chosen randomly or as a contiguous burst, while keeping PL headers
and pilots. LeanDVB drops exactly those frames; public unpack recovers the
original 63,497/85,161 bytes and verifies SHA-256 in all six clean/loss cases.
Decoded-frame injection also checks exactly M erasures, loss of packet zero,
and rejection at M+1. This establishes erasure recovery, not RF reception or
carrier-blackout reacquisition: blanking entire IQ intervals caused additional
LeanDVB losses beyond the budget in a host trial.

Sending spools the snapshot and coded stream to temporary files and buffers one
coding block. Recovery currently buffers the received transfer and reconstructed
container in memory. Neither path establishes real-time encoding throughput.

## Coordinator only: radio off first

Use an already flashed compatible checkout; these are the only board steps,
to be run by the coordinator. In a host shell, from the repository root:

```sh
PORT=/dev/cu.usbmodem101
RX=/path/to/leandvb
mkdir -p notes/filelink
python3 -c 'from pathlib import Path; Path("notes/filelink/input.bin").write_bytes(bytes(range(256))*32)'
python3 examples/usb-send/send.py send notes/filelink/input.bin --port "$PORT" \
  --fec 16:4 --modcod 4 --rate 1m --dump notes/filelink/file.iq --frames 48 \
  --report notes/filelink/dump.json
python3 host/filelink_check.py notes/filelink/input.bin notes/filelink/file.iq \
  notes/filelink/dump.json --receiver "$RX"
```

The offline checker opens no port. It requires successful finite input, matching
board/USB/IQ CRCs and lengths, LeanDVB GCS lock and zero clean-channel FEC errors,
then checks full recovery, random M-loss and burst M-loss recovery per block,
and rejection at M+1 with no output. Losses here are injected after decoding the
board dump. It prints JSON evidence. Use fresh IQ/report paths for another run.
This board check is provided for the coordinator; it has not been run here.

## Coordinator only: keyed, closed Faraday box

With both devices/antennas inside the **closed box**, compatible firmware already
flashed, and an assigned callsign stored, run from the repository root:

```sh
python3 examples/over-the-air/file_check.py tests/data/file-payload.png notes/file-ota \
  --port /dev/cu.usbmodem1101 --uri ip:192.168.1.10 --closed-box --key
```

Defaults: real 237,171-byte PNG, FEC 16:4, QPSK 3/4 at 5 Msym/s, 2420 MHz,
TX gain 0/peak 128, manual RX gain 30 dB, 30 seconds including both Morse IDs.
Choose the actual port and authorized frequency (`--port`/`--mhz`);
`--modcod 4` selects QPSK 1/2. Override the existing LeanDVB/Radioconda paths
with `--receiver`/`--iio-python`; see the [RF checker](../examples/over-the-air/README.md).
Allow 5.15 GB free disk and use a fresh output directory.

The coordinator snapshots the file, reserves the exact serial port without reset,
validates the stored callsign/airtime budget, and starts the existing receive-only
libiio worker with fail-closed TX inhibits. After one second of IQ it runs the
USB FEC sender once, requires complete file acceptance/playback and healthy
counters, then decodes with LeanDVB and the public `unpack --fec --bbf` path.
It never flashes or writes callsigns. Ctrl-C cannot stop already keyed firmware;
keep the box closed until the timed transmission ends.

Two recovery runs use the same RF capture: `received` keeps all decoded frames;
`dropped` discards one received source frame per block, including duplicates,
then unpacks again. `--drop-per-block N` increases the injection. Both original
file SHA-256 values must match. Each arm prints **sent, lost (channel + injected),
source shards recovered, and remaining erasure frames per block**. Margin is
`distinct received shards - K`; zero passes. Channel loss consumes the same
four-erasure budget, so injection may fail after normal recovery succeeds.
Entire missing blocks count; guards/dummy positions are reported separately.

`PASS`/exit 0 requires both recoveries, instrumented GCS lock in the requested
mode, healthy board counters, complete capture and TX inhibit readbacks.
Failures exit nonzero and preserve `report.json`, USB/capture/decoder evidence,
and received/dropped BBFRAMEs. libiio's unavailable overflow counter stays `null`.
Replace `--closed-box --key` with `--plan` for a device-free plan, or with
`--recheck notes/file-ota` and a fresh output directory for an offline BBFRAME
audit using the same source/settings.

Host tests: `python3 tests/test_file_ota.py`; add `--receiver /path/to/leandvb`
for model IQ through an independent decoder. These pass with the real PNG,
including one source-frame drop in each of three blocks (three erasures of
margin left per block). This is **offline evidence**; the closed-box RF run
still needs operator evidence.
