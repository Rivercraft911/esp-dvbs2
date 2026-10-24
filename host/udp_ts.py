#!/usr/bin/env python3
"""Receive a transport stream over UDP and check it: what the receiver's W5500 sends to the
laptop, from `rx ... ts eth` or from the `eth send` diagnostic.

    caffeinate -i -s -d python3 host/udp_ts.py --port 1234 --seconds 60 --save run.ts
    caffeinate -i -s -d python3 host/udp_ts.py --port 1234 --diag --out notes/eth.jsonl

Prints one JSON summary: datagrams and their sizes, TS packets, payload rate, sync errors,
packets marked bad by the receiver's CRC-8 (transport_error_indicator), continuity errors per PID
and the longest gap between datagrams. With --diag the datagrams are `eth send`'s numbered ones
(w5500_diag_pattern: the number is bytes 4-7 of each, MSB first), and lost or reordered numbers
are counted too. It stops after --seconds, or once nothing has arrived for --idle seconds after
the first datagram. --save appends the payloads to a .ts file. With --pattern replay every packet
must equal one of the 56 packets the transmitter's `spiinput ... ts replay` loops
(host/spi_input.py pattern, sequences 0 to 7); the first bad ones are listed with their datagram
number and position in it. --forward HOST:PORT relays every datagram on (to pv view, say) while
timing its arrival: the summary then has gap percentiles, the longest gap and the gaps over 20
and 100 ms.

Run it under caffeinate: without it this Mac stalls USB and network readers for about 1.25 s
roughly once a minute.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import socket
import sys
import time

TS = 188


def replay_packets() -> set[bytes]:
    """The 56 packets of the transmitter's TS replay loop."""
    from spi_input import pattern
    cycle = b"".join(bytes(pattern(seq, 1)) for seq in range(8))
    return {cycle[k:k + TS] for k in range(0, len(cycle), TS)}


class Check:
    """Counters over a run of datagrams, fed one at a time with their arrival times."""

    def __init__(self, diag: bool = False, packets: set[bytes] | None = None):
        self.diag = diag
        self.expected = packets
        self.pattern_bad, self.bad_at = 0, []
        self.datagrams = self.bytes = self.packets = 0
        self.sizes: dict[int, int] = {}
        self.sync_errors = self.tei = self.cc_errors = 0
        self.pids: dict[int, list[int]] = {}    # pid: [packets, continuity errors, last cc]
        self.first = self.last = None
        self.max_gap = 0.0
        self.gaps: list[float] = []
        self.long: list[list[float]] = []   # [seconds into the run, gap ms] for gaps over 20 ms
        self.seq = self.first_seq = None
        self.lost = self.reordered = 0

    def add(self, d: bytes, t: float) -> None:
        if self.last is not None:
            self.max_gap = max(self.max_gap, t - self.last)
            self.gaps.append(t - self.last)
            if t - self.last > 0.020 and len(self.long) < 200:
                self.long.append([round(t - self.first, 3), round((t - self.last) * 1e3, 1)])
        self.first = t if self.first is None else self.first
        self.last = t
        self.datagrams += 1
        self.bytes += len(d)
        self.sizes[len(d)] = self.sizes.get(len(d), 0) + 1
        for k in range(0, len(d) - TS + 1, TS):
            self.packet(d[k:k + TS])
            if self.expected is not None and d[k:k + TS] not in self.expected:
                self.pattern_bad += 1
                if len(self.bad_at) < 40:
                    self.bad_at.append([self.datagrams - 1, k // TS, len(d), d[k:k + 8].hex()])
        self.sync_errors += len(d) % TS != 0    # a torn datagram counts once
        if self.diag and len(d) >= 8:
            seq = int.from_bytes(d[4:8], "big")
            self.first_seq = seq if self.first_seq is None else self.first_seq
            if self.seq is not None:
                step = (seq - self.seq) & 0xFFFFFFFF
                if step == 0 or step > 0x7FFFFFFF:
                    self.reordered += 1
                    return
                self.lost += step - 1
            self.seq = seq

    def packet(self, p: bytes) -> None:
        self.packets += 1
        if p[0] != 0x47:
            self.sync_errors += 1
            return
        self.tei += p[1] >> 7
        pid, cc, payload = (p[1] & 0x1F) << 8 | p[2], p[3] & 15, p[3] & 0x10
        if pid == 0x1FFF:
            return
        entry = self.pids.setdefault(pid, [0, 0, None])
        entry[0] += 1
        if entry[2] is not None and payload and cc != (entry[2] + 1) & 15:
            entry[1] += 1
            self.cc_errors += 1
        if payload or entry[2] is None:
            entry[2] = cc

    def summary(self) -> dict:
        span = (self.last - self.first) if self.datagrams > 1 else 0.0
        out = dict(datagrams=self.datagrams, bytes=self.bytes, packets=self.packets,
                   mbps=round(self.bytes * 8 / span / 1e6, 3) if span else None,
                   seconds=round(span, 3), sizes={str(k): v for k, v in sorted(self.sizes.items())},
                   sync_errors=self.sync_errors, tei=self.tei, cc_errors=self.cc_errors,
                   pids={f"0x{p:04x}": dict(packets=v[0], cc_errors=v[1]) for p, v in sorted(self.pids.items())},
                   max_gap_ms=round(self.max_gap * 1e3, 2))
        if self.gaps:
            g = sorted(self.gaps)
            pick = lambda q: round(g[min(len(g) - 1, int(q * len(g)))] * 1e3, 2)
            out |= dict(gap_ms=dict(median=pick(0.5), p99=pick(0.99), p999=pick(0.999)),
                        gaps_over_20ms=sum(x > 0.020 for x in g), gaps_over_100ms=sum(x > 0.100 for x in g),
                        long_gaps=self.long)
        if self.expected is not None:
            out |= dict(pattern_bad=self.pattern_bad, bad_at=self.bad_at)
        if self.diag:
            out |= dict(lost=self.lost, reordered=self.reordered, first_seq=self.first_seq,
                        last_seq=self.seq)
        return out


def receive(sock: socket.socket, seconds: float, idle: float, check: Check, save=None,
            forward=None) -> Check:
    """Datagrams from a bound socket until seconds pass, or idle seconds after the last one;
    forward: an (address, port) each is relayed to."""
    sock.settimeout(0.05)
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        try:
            d = sock.recv(65535)
        except socket.timeout:
            if check.last is not None and time.monotonic() - check.last > idle:
                break
            continue
        check.add(d, time.monotonic())
        if forward:
            sock.sendto(d, forward)
        if save:
            save.write(d)
    return check


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=1234, help="UDP port to listen on")
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--idle", type=float, default=3.0, help="stop this long after the last datagram")
    p.add_argument("--diag", action="store_true", help="`eth send` datagrams: count lost numbers")
    p.add_argument("--pattern", choices=("replay",), help="check every packet against this input")
    p.add_argument("--save", type=pathlib.Path, help="append the payloads to this .ts file")
    p.add_argument("--out", type=pathlib.Path, help="append the summary to this JSON-lines file")
    p.add_argument("--forward", metavar="HOST:PORT", help="relay every datagram to HOST:PORT")
    a = p.parse_args()
    forward = None
    if a.forward:
        host, port = a.forward.rsplit(":", 1)
        forward = (host, int(port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    sock.bind(("", a.port))
    save = a.save.open("ab") if a.save else None
    try:
        packets = replay_packets() if a.pattern == "replay" else None
        summary = receive(sock, a.seconds, a.idle, Check(a.diag, packets), save, forward).summary()
    finally:
        if save:
            save.close()
    summary = dict(port=a.port, rcvbuf=sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)) | summary
    print(json.dumps(summary))
    if a.out:
        stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
        with a.out.open("a") as f:
            f.write(json.dumps({"host_time": stamp, **summary}) + "\n")
    return 0 if summary["datagrams"] else 1


if __name__ == "__main__":
    sys.exit(main())
