#!/usr/bin/env python3
"""Pure Python test video: 160x96 MPEG-2 I pictures, 25 fps, in a 9 Mb/s TS.

Animated gray bars, no audio. No camera, external codec, or GStreamer required.
MPEG-2 DC-only intra blocks follow ITU-T H.262; PSI/PES follow H.222.0.
"""
import argparse
import struct


def crc32(data):
    """MPEG-2 PSI CRC (non-reflected, init ffffffff, no final XOR)."""
    crc = 0xffffffff
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc


def bits(*fields):
    text = "".join(f"{value:0{width}b}" for value, width in fields)
    text += "0" * (-len(text) % 8)
    return int(text, 2).to_bytes(len(text) // 8, "big")


def start(code, payload):
    return b"\0\0\1" + bytes([code]) + payload


def picture(frame):
    seq = start(0xb3, bits((160, 12), (96, 12), (1, 4), (3, 4),
                          (0x3ffff, 18), (1, 1), (112, 10), (0, 3)))
    seq += start(0xb5, bits((1, 4), (0x48, 8), (1, 1), (1, 2), (0, 4),
                            (0, 12), (1, 1), (0, 8), (0, 1), (0, 2), (0, 5)))
    pic = start(0, bits((frame % 1024, 10), (1, 3), (0xffff, 16), (0, 1)))
    pic += start(0xb5, bits((8, 4), (0xffff, 16), (0, 2), (3, 2),
                            (0, 1), (1, 1), (0, 5), (1, 1), (1, 1), (0, 1)))
    # Differential DC VLCs, H.262 Tables B.12/B.13; all AC coefficients are zero.
    dc_luma = ("100", "00", "01", "101", "110", "1110", "11110", "111110", "1111110")
    for row in range(6):
        text = "01000" + "0"  # quantiser_scale_code=8, extra_bit_slice=0
        predictor = 128
        for column in range(10):
            level = 48 + ((column + frame) % 10) * 16
            text += "11"  # macroblock_address_increment=1, intra macroblock
            for block in range(4):
                diff = level - predictor
                predictor = level
                size = abs(diff).bit_length()
                text += dc_luma[size]
                if size:
                    text += f"{diff if diff > 0 else diff + (1 << size) - 1:0{size}b}"
                text += "10"  # end_of_block
            text += "0010" * 2  # Cb/Cr DC size=0, end_of_block
        text += "0" * (-len(text) % 8)
        pic += start(row + 1, int(text, 2).to_bytes(len(text) // 8, "big"))
    return seq + pic


def pts(value):
    return bytes([0x21 | ((value >> 29) & 14), (value >> 22) & 255,
                  1 | ((value >> 14) & 254), (value >> 7) & 255, 1 | ((value << 1) & 254)])


def make_ts(frames=25, muxrate=9_000_000):
    counters = {}
    output = bytearray()

    def packet(pid, payload, first=False, pcr=None):
        cc = counters.get(pid, 0)
        counters[pid] = (cc + 1) & 15
        header = bytes([0x47, (pid >> 8) | (0x40 if first else 0), pid & 255])
        if pcr is not None:
            base = pcr // 300
            clock = bytes([(base >> 25) & 255, (base >> 17) & 255,
                           (base >> 9) & 255, (base >> 1) & 255,
                           ((base & 1) << 7) | 0x7e | ((pcr % 300) >> 8), pcr % 300 & 255])
            adaptation = b"\x10" + clock
        else:
            adaptation = b""
        take = min(len(payload), 183 - len(adaptation)) if adaptation else min(len(payload), 184)
        if adaptation or take < 184:
            n = 183 - take
            field = (adaptation or b"\0") if n else b""
            output.extend(header + bytes([0x30 | cc, n]) + field + b"\xff" * (n - len(field)) + payload[:take])
        else:
            output.extend(header + bytes([0x10 | cc]) + payload[:take])
        return payload[take:]

    def section(pid, data):
        packet(pid, b"\0" + data + struct.pack(">I", crc32(data)), True)

    for frame in range(frames):
        # Program 1, PMT PID 4096, MPEG-2 video/PCR PID 256.
        section(0, bytes.fromhex("00b00d0001c100000001f000"))
        section(4096, bytes.fromhex("02b0120001c10000e100f00002e100f000"))
        payload = b"\0\0\1\xe0\0\0\x80\x80\x05" + pts(90000 + frame * 3600) + picture(frame)
        first = True
        while payload:
            # PCR follows byte position at the declared constant mux rate.
            pcr = len(output) * 8 * 27_000_000 // muxrate if first else None
            payload = packet(256, payload, first, pcr)
            first = False
        target = (frame + 1) * muxrate // (25 * 8 * 188)
        while len(output) // 188 < target:
            packet(8191, b"\xff" * 184)
    return bytes(output)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output")
    p.add_argument("--frames", type=int, default=25)
    a = p.parse_args()
    if a.frames < 1:
        p.error("frames must be positive")
    with open(a.output, "xb") as f:
        f.write(make_ts(a.frames))
