#!/usr/bin/env python3
"""Send raw PV-SPI v1 from a TS/GCS file or stdin. No hardware imports for --vector."""
import argparse
import json
from pathlib import Path
import struct
import sys
import time
import zlib


def message(payload, seq, kind=1):
    if kind not in (0, 1, 2, 3) or len(payload) > 1316:
        raise ValueError("invalid PV type or payload length")
    if (kind == 0 and payload) or (kind != 0 and not payload):
        raise ValueError("NOP must be empty; data must be nonempty")
    if kind == 1:
        if len(payload) % 188:
            raise ValueError("need 1..7 complete TS packets")
        if any(payload[i] != 0x47 for i in range(0, len(payload), 188)):
            raise ValueError("TS sync byte missing")
    if kind == 3 and (len(payload) != 1 or not 4 <= payload[0] <= 17):
        raise ValueError("control needs one MODCOD byte, 4..17")
    body = struct.pack("<HBBHHI", 0x5650, 1, kind, seq & 65535, len(payload), 0)
    body += payload + bytes(1316-len(payload))
    return body + struct.pack("<I", zlib.crc32(body))


def payloads(source, kind=1):
    # Pipe reads may be short. Accumulate until a full batch or EOF.
    pending = b""
    while True:
        part = source.read(1316-len(pending))
        if part:
            pending += part
            if len(pending) < 1316:
                continue
        elif not pending:
            return
        message(pending, 0, kind)  # Reject a partial TS packet at EOF.
        yield pending
        pending = b""
        if not part:
            return


def send(source, spi, ready, stats, *, kind=1, mbps=9., timeout=2., clock=time):
    released = clock.monotonic()
    for payload in payloads(source, kind):
        wire = message(payload, stats['next_seq'], kind)
        tx = list(wire)  # Prepare the transfer before sampling READY.
        delay = released + 10e-6 - clock.monotonic()
        if delay > 0:
            clock.sleep(delay)
        started = clock.monotonic()
        waited = False
        while True:
            if waited and clock.monotonic()-started >= timeout:
                stats['ready_timeouts'] += 1
                raise TimeoutError(f"READY low for {timeout:g} seconds")
            if ready():
                break
            if not waited:
                stats['ready_waits'] += 1
                waited = True
            clock.sleep(20e-6)
        stats['attempted_messages'] += 1
        try:
            # One synchronous xfer2, one CS assertion, exactly 1332 bytes.
            result = spi.xfer2(tx)
            if len(result) != 1332:
                raise OSError("short SPI transfer; do not retry")
        except BaseException:
            stats['transfer_uncertain'] = True
            raise
        released = clock.monotonic()
        stats['messages'] += 1
        stats['ts_packets'] += len(payload)//188 if kind == 1 else 0
        stats['payload_bytes'] += len(payload)
        if kind == 1:
            stats['crc_chain'] = zlib.crc32(wire[-4:], stats['crc_chain'])
        stats['next_seq'] = (stats['next_seq']+1) & 65535
        delay = len(payload)*8/(mbps*1e6) - (released-started)
        if delay > 0:
            clock.sleep(delay)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('file', nargs='?', help='input file, or - for stdin')
    p.add_argument('--kind', choices=('ts', 'gcs'), default='ts')
    p.add_argument('--vector', action='store_true')
    p.add_argument('--gpiochip', default='/dev/gpiochip0', help='RP1 chip from gpioinfo')
    p.add_argument('--ready', type=int, default=25)
    p.add_argument('--hz', type=int, default=20000000)
    p.add_argument('--mbps', type=float, default=9., help='offered payload rate ceiling')
    a = p.parse_args()
    if a.vector:
        wire = message(bytes((0x47, 1, 0, 0x10))+bytes(range(184)), 1)
        print(f"{len(wire)} bytes, CRC 0x{int.from_bytes(wire[-4:], 'little'):08x}")
        return
    if not a.file or not 1 <= a.hz <= 20000000 or not 0 < a.mbps <= 20:
        p.error('supply a file or -, 1..20000000 Hz, and 0 < Mb/s <= 20')
    import gpiod
    from gpiod.line import Direction, Value
    import spidev
    stats = dict(messages=0, attempted_messages=0, ts_packets=0, payload_bytes=0,
                 crc_chain=0, next_seq=0, ready_waits=0, ready_timeouts=0,
                 transfer_uncertain=False)
    with gpiod.request_lines(a.gpiochip, consumer='pv-spi', config={
            a.ready: gpiod.LineSettings(direction=Direction.INPUT)}) as request:
        with spidev.SpiDev(0, 0) as spi:
            spi.mode, spi.bits_per_word, spi.lsbfirst = 0, 8, False
            spi.max_speed_hz = a.hz
            source = sys.stdin.buffer if a.file == '-' else Path(a.file).open('rb')
            try:
                send(source, spi, lambda: request.get_value(a.ready) == Value.ACTIVE,
                     stats, kind=1 if a.kind == 'ts' else 2, mbps=a.mbps)
            finally:
                if source is not sys.stdin.buffer:
                    source.close()
                print(json.dumps(stats), file=sys.stderr)


if __name__ == '__main__':
    main()
