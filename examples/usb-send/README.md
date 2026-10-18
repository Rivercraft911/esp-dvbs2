# USB sender

One Python file sends video or files from a laptop to the S3's **native
USB-Serial-JTAG** port. The S3 frames and encodes them. Copy `send.py` anywhere;
its only dependency is `pyserial`. Close other serial tools first.

```sh
python3 -m pip install pyserial
python3 send.py send movie.ts --port /dev/cu.usbmodem101
python3 send.py send document.pdf --seconds 30
ffmpeg -re -i movie.mp4 -c:v libx264 -b:v 500k -maxrate 500k -bufsize 1M \
  -an -f mpegts - | python3 send.py send - --kind ts --seconds 30
cat bytes.bin | python3 send.py send - --kind gcs
```

Default: **radio-off `dvbpace`, QPSK 1/2 at 1 Msym/s**, ten seconds, 0.965 Mb/s
payload. `.ts` files and stdin default to TS; other files get a GCS file container.
`--kind ts|gcs|file` overrides this. TS requires aligned 188-byte packets; GCS
passes any bytes unchanged. `--kind file --name capture.bin` also accepts stdin,
spooling before the run to calculate its length and hash. Memory use is bounded.
Without `--port`, one unambiguous Espressif native USB port is selected; baud rate
is ignored. Linux and Windows port names work too.

`--modcod`, `--rate`, `--seconds`, and optional `--mbps` choose the mode, duration
and offered payload ceiling; see `send --help`. USB backpressure normally paces
sending. Timed senders finish two seconds early so input can drain. Longer files
need more seconds; incomplete containers fail. Raw streams can leave a partial
BBFRAME at EOF; containers pad it. Dummy frames fill gaps when input runs dry.

RF requires explicit `--key --frequency 2400 --seconds 20` (2390..2450 integer
MHz; optional `--gain 0..17`, default 0). The script reads `info` and refuses RF
without a **stored assigned callsign**, set via console `callsign YOURCALL`.
Firmware retains its 60-second keyed limit and start/end Morse station ID.

## File recovery

```sh
python3 send.py send document.pdf --dump document.iq --frames 24 --report dump.json
# Decode the int16-LE IQ with LeanDVB; the coordinator below automates this.
python3 send.py unpack decoded.gcs -o recovered.pdf
```

The container is `PVFILE1\n`, little-endian uint16 UTF-8 basename length, uint64
file length, 32 SHA-256 bytes, basename, then file bytes. Eight zero-filled
BBFRAMEs precede it for acquisition; padding completes the last frame and adds
four trailing frames. `unpack` scans decoded **GCS DATA FIELD bytes**, checks SHA-256
before creating output, rejects paths and refuses overwrites. For GCS, use
LeanDVB `--fd-bbf` and strip each ten-byte BBHEADER, as `host/decode.py` does;
LeanDVB's normal stdout is TS-only. Dumps allow 1..256 frames and run slower than
real time over USB. Increase the frame count for larger files.

## Coordinator: radio-off proof, about two minutes

Already flashed with this branch, native USB plugged in, other consoles closed.
From the repository root, the **coordinator** runs:

```sh
PORT=/dev/cu.usbmodem101 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
python3 examples/usb-send/check.py
```

No flashing, reset or RF command. Evidence goes under `notes/usb-board-*`:

- Send a 2 KiB file, dump 24 frames, verify USB dump CRC, decode with independent
  LeanDVB, run `unpack`, require identical SHA-256 and zero reported FEC errors.
- Saturate USB GCS for 20 seconds using 8PSK 2/3 at 5 Msym/s (9.678 Mb/s demand).
  Report measured payload Mb/s; require at least 3.5 Mb/s over 15 seconds.
- Pace TS for 20 seconds at QPSK 1/2's 0.965 Mb/s payload rate; require at least 98%.

Both paces require matched sender/board counts, zero protocol/sequence errors and
no missed output bank deadlines. Dummies at startup/end or during saturation
are expected. Checks continue after failures; `summary.json` records the outcome.
**USB+encoder throughput is not yet measured on this branch.** Archived USB-only
intake was 4.6 Mb/s intact; roughly 4 Mb/s with encoding is an expectation.

Host tests never open ports: `python3 tests/test_usb_input.py` and
`python3 tests/test_usb_send.py`. Add `--receiver "$RX"` to the latter for a C
encoder → LeanDVB → unpack/hash proof, independent of hardware.

`spiinput usb [ts|gcs]` selects USB for the next run. The sender handles the
ready/stop/NOP handoff, reads IQ and console results concurrently, and checks
receiver counters. See the [console protocol](../../docs/spi-input.md#usb-payload-on-the-console).
After an interrupted or uncertain write, manually reset before a fresh session;
firmware quarantines unfinished binary input with the radio off.
