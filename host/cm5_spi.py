"""CM5 single-line PV-SPI TS sender. SPI/READY are opened only by the send command."""
import argparse
from pathlib import Path
import struct
import sys
import time
import zlib


def message(payload, seq):
    if not payload or len(payload) > 1316 or len(payload) % 188:
        raise ValueError('send 1..7 whole TS packets')
    if any(payload[i] != 0x47 for i in range(0, len(payload), 188)):
        raise ValueError('TS sync lost')
    body = struct.pack('<2sBBHHI', b'PV', 1, 1, seq & 65535, len(payload), 0)
    body += payload + bytes(1316-len(payload))
    # Wr_DMA, address zero, eight dummy clocks, then the PV message, all on MOSI.
    return b'\x03\0\0' + body + struct.pack('<I', zlib.crc32(body))


def send(source, spi, ready, mbps=22, clock=time):
    """One synchronous CS assertion per fresh READY sample; no queued transactions."""
    seq = total = 0
    while True:
        started = clock.monotonic()
        payload = source.read(1316)
        if not payload:
            return total
        wire = message(payload, seq)
        # The previous ioctl has returned with CS high. Never use a READY sample
        # taken before this inter-message gap, or combine messages into one ioctl.
        clock.sleep(10e-6)
        while not ready():
            clock.sleep(20e-6)
        spi.xfer2(list(wire))
        total += len(payload)
        seq += 1
        # Fixed offered-rate ceiling, including transfer time.
        elapsed = clock.monotonic()-started
        wait = len(payload)*8/(mbps*1e6)-elapsed
        if wait > 0:
            clock.sleep(wait)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('file', help='MPEG-TS file or - for stdin; producer supplies null padding')
    p.add_argument('--gpiochip', default='/dev/gpiochip0')
    p.add_argument('--ready', type=int, default=25, help='RP1 line offset, not header pin')
    p.add_argument('--bus', type=int, default=0)
    p.add_argument('--device', type=int, default=0)
    p.add_argument('--hz', type=int, default=25000000)
    a = p.parse_args()
    if not 1 <= a.hz <= 40000000:
        p.error('clock must be 1..40000000 Hz')
    import gpiod
    from gpiod.line import Direction, Value
    import spidev
    with gpiod.request_lines(a.gpiochip, consumer='pv-spi-ready', config={
            a.ready: gpiod.LineSettings(direction=Direction.INPUT)}) as request:
        with spidev.SpiDev(a.bus, a.device) as spi:
            spi.mode = 0
            spi.max_speed_hz = a.hz
            spi.bits_per_word = 8
            spi.lsbfirst = False
            source = sys.stdin.buffer if a.file == '-' else Path(a.file).open('rb')
            try:
                count = send(source, spi, lambda: request.get_value(a.ready) == Value.ACTIVE)
                print(f'{count} TS bytes sent', file=sys.stderr)
            finally:
                if source is not sys.stdin.buffer:
                    source.close()


if __name__ == '__main__':
    main()
