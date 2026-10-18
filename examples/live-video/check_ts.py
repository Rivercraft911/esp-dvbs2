#!/usr/bin/env python3
"""Check TS framing, PID continuity, PAT/PMT CRCs, video PES and PCR presence.

An optional reference must contain the entire received packet sequence as an
exact contiguous substream: acquisition/EOF trimming is allowed, interior loss
is not. This does not measure PCR jitter or decode video.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

from make_ts import crc32

VIDEO_TYPES = {0x01, 0x02, 0x10, 0x1b, 0x24}


def check(data, reference=None):
    if not data or len(data) % 188:
        raise ValueError("TS must be nonempty and a multiple of 188 bytes")
    counts, cc, previous, sections = Counter(), {}, {}, {}
    programs, streams, pcr_pids = {}, {}, set()
    pes, pcr = Counter(), Counter()
    tables = Counter()
    duplicates = 0

    def parse_section(pid, section):
        if crc32(section):
            raise ValueError(f"PSI CRC error on PID {pid}")
        if not section[5] & 1:  # current_next_indicator
            return
        if pid == 0 and section[0] == 0:
            if len(section) < 12 or (len(section) - 12) % 4:
                raise ValueError("malformed PAT")
            for k in range(8, len(section) - 4, 4):
                program = int.from_bytes(section[k:k+2], "big")
                if program:
                    programs[program] = int.from_bytes(section[k+2:k+4], "big") & 8191
            tables["PAT"] += 1
        elif pid in programs.values() and section[0] == 2:
            if len(section) < 16:
                raise ValueError("short PMT")
            program = int.from_bytes(section[3:5], "big")
            if programs.get(program) != pid:
                raise ValueError("PMT program does not match PAT")
            pcr_pids.add(int.from_bytes(section[8:10], "big") & 8191)
            k = 12 + (int.from_bytes(section[10:12], "big") & 4095)
            while k < len(section) - 4:
                if k + 5 > len(section) - 4:
                    raise ValueError("truncated PMT stream")
                streams[int.from_bytes(section[k+1:k+3], "big") & 8191] = section[k]
                k += 5 + (int.from_bytes(section[k+3:k+5], "big") & 4095)
            if k != len(section) - 4:
                raise ValueError("bad PMT descriptor length")
            tables["PMT"] += 1
        else:
            raise ValueError(f"unexpected PSI table on PID {pid}")

    def drain(pid):
        buf = sections[pid]
        while len(buf) >= 3:
            if buf[0] == 255:
                buf.clear()
                break
            n = 3 + (int.from_bytes(buf[1:3], "big") & 4095)
            if n < 12 or n > 1024:
                raise ValueError("invalid PSI section length")
            if len(buf) < n:
                break
            parse_section(pid, bytes(buf[:n]))
            del buf[:n]

    for offset in range(0, len(data), 188):
        packet = data[offset:offset+188]
        if packet[0] != 0x47 or packet[1] & 0x80 or packet[3] & 0xc0:
            raise ValueError(f"sync/TEI/scrambling error at byte {offset}")
        pid = ((packet[1] & 31) << 8) | packet[2]
        counts[pid] += 1
        afc, counter, pos = (packet[3] >> 4) & 3, packet[3] & 15, 4
        if not afc:
            raise ValueError("reserved adaptation control")
        if afc & 2:
            size = packet[pos]
            pos += size + 1
            if pos > 188 or (afc == 2 and pos != 188):
                raise ValueError("bad adaptation field length")
            if size:
                flags = packet[5]
                if flags & 0x80:
                    raise ValueError(f"declared discontinuity on PID {pid}")
                if flags & 0x10:
                    if size < 7:
                        raise ValueError("short PCR")
                    pcr[pid] += 1
        if pid != 8191:
            if pid in cc:
                if counter == cc[pid] and packet == previous[pid] and afc & 1:
                    duplicates += 1
                    continue
                expected = (cc[pid] + bool(afc & 1)) & 15
                if counter != expected:
                    raise ValueError(f"continuity PID {pid}: expected {expected}, got {counter}, byte {offset}")
            cc[pid], previous[pid] = counter, packet
        if not afc & 1:
            continue
        if pos >= 188:
            raise ValueError("payload flag without payload")
        payload = packet[pos:]
        first = bool(packet[1] & 0x40)
        if first and payload.startswith(b"\0\0\1"):
            pes[pid] += 1
        if pid == 0 or pid in programs.values():
            if first:
                pointer = payload[0]
                if pointer + 1 > len(payload):
                    raise ValueError("bad PSI pointer")
                if sections.get(pid):
                    sections[pid].extend(payload[1:1+pointer])
                    drain(pid)
                    if sections[pid]:
                        raise ValueError("unfinished PSI section before new start")
                sections[pid] = bytearray(payload[1+pointer:])
            elif pid in sections:
                sections[pid].extend(payload)
            if pid in sections:
                drain(pid)
    video = sorted(pid for pid, typ in streams.items() if typ in VIDEO_TYPES)
    if not tables["PAT"] or not tables["PMT"] or not video:
        raise ValueError("missing PAT, PMT or declared video stream")
    if any(not counts[pid] or not pes[pid] for pid in video):
        raise ValueError("declared video PID missing packets or PES starts")
    if not any(pcr[pid] for pid in pcr_pids):
        raise ValueError("missing PCR on a PMT-declared PCR PID")
    report = {"packets": len(data) // 188, "pids": dict(sorted(counts.items())),
              "programs": programs, "stream_types": streams, "video_pids": video,
              "tables": dict(tables), "pcr": dict(pcr), "pes_starts": dict(pes),
              "duplicates": duplicates, "continuity_errors": 0, "psi_crc_errors": 0}
    if reference is not None:
        match = reference.find(data)
        if match < 0 or match % 188:
            raise ValueError("received TS is not an exact contiguous slice of the source")
        if len(data) < 100 * 188:
            raise ValueError("too few packets for reference comparison")
        report.update(reference_offset_packets=match // 188, exact_reference_match=True)
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("ts", type=Path)
    p.add_argument("--reference", type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(check(a.ts.read_bytes(), a.reference.read_bytes() if a.reference else None), indent=2))
    except (ValueError, OSError) as e:
        p.exit(1, f"TS check: {e}\n")
