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
sending. Timed senders finish two payload seconds early so input can drain;
radio-off pace adds the opening and any periodic Morse ID to the send window.
Write deadlines cover those pauses, including the terminal NOP. Longer files
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

No flashing or RF command. The recovery check deliberately triggers a firmware
restart after RF-off counters are saved. Evidence goes under `notes/usb-board-*`:

- Send a 2 KiB file, dump 24 frames, verify USB dump CRC, decode with independent
  LeanDVB, run `unpack`, require identical SHA-256 and zero reported FEC errors.
- Saturate USB GCS for 20 seconds using 8PSK 2/3 at 5 Msym/s (9.678 Mb/s demand).
  Report measured payload Mb/s; require at least 3.5 Mb/s over 15 complete
  seconds after the opening ID.
- Interrupt a PV message after 17 bytes, require a failure report with board
  counters and an automatic restart, then open a fresh console for the next check.
- Pace TS for 20 seconds at QPSK 1/2's 0.965 Mb/s payload rate; require at least 98%.

Both paces require matched sender/board counts, zero protocol/sequence errors and
no missed output bank deadlines. Dummies at startup/end or during saturation
are expected. Checks continue after failures; `summary.json` records the outcome.
**Sustainable USB+encoder throughput still needs a board rerun.** Archived
USB-only intake was 4.6 Mb/s intact. The host scheduling model reproduces the
previous failure with ideal task scheduling: the encoder fills its output ring
during N0CALL's 5.22-second opening ID, while the radio-off sender abandons its
blocked write after five seconds. The longer deadline survives this pause; the
model then delivers 3.79 Mb/s GCS and 0.966 Mb/s TS with identical payloads.
These are simulations, not board measurements.

Host tests never open ports: `python3 tests/test_usb_input.py` and
`python3 tests/test_usb_send.py`. Add `--receiver "$RX"` to the latter for a C
encoder → LeanDVB → unpack/hash proof, independent of hardware. The RX model also
injects a late driver-queue refill after the initial handoff, as well as the CW
pause with two/eight output slots and a finite host write deadline. The sender's
Morse calculation is checked against the actual C generator at both DAC rates.

RX runs inside the encoder on core 0 (priority 5 for these modes). Empty-input
waits sleep on a timer notification; full-output waits sleep on slot notifications.
The timed consumer runs on core 1 and emits dummies instead of waiting on input.
There is no separate lower-priority reader for the encoder to starve.
The original `usb.us` stops at the last complete message even when empty reads
continue. Input cycle counters include notification sleeps inside the read call;
they do not measure CPU busy time.

`spiinput usb [ts|gcs]` selects USB for the next run. The sender handles the
ready/stop/NOP handoff, reads IQ and console results concurrently, and checks
receiver counters. See the [console protocol](../../docs/spi-input.md#over-usb-instead).
A missing NOP triggers an automatic restart three seconds after the run ends,
with RF already off. The sender keeps reading after a write failure to collect
final counters and recovery status, and retries its read-only `info` probe across
USB re-enumeration. It never retries a write with an uncertain byte count.

Every send writes a report, including on failure: `usb-send-report.json` by
default, or `--report PATH`. Reports include confirmed host write progress,
uncertain-write status, FIFO/driver byte counts, reader waits, encoder results,
and recovery status. `rx_reenabled` counts RX-enable bits restored during a run;
`driver_queue_max` and `driver_pending` expose queued bytes, and `intr_ena` records
the final interrupt-enable register. `usb.trace[i]` contains RX read calls and
payload bytes in second `i` after ready, for the first 64 seconds. Expect empty
bins while CW holds a full output ring, then regular activity after the ID.
`last_message_us` marks the final complete message relative to ready.
The summary's `payload_mbps` uses complete seconds after ID plus startup allowance,
ending before that final message; `measurement_us` and `measurement_payload_bytes`
give its denominator and numerator. `wall_payload_mbps` retains the original rate
including the pause. Both rate checks require at least 15 measured seconds.
FIFO plus driver bytes must equal wire bytes. If the board disconnects before counters can be read,
`board_counters_available` is false; host diagnostics are still saved.
