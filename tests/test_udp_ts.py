"""Check host/udp_ts.py, the laptop end of the W5500 egress, on made-up datagrams and over a
loopback socket.

    python3 tests/test_udp_ts.py
"""
import pathlib
import socket
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "host"))
import udp_ts  # noqa: E402


def diag(seq):
    """`eth send` datagram seq, as w5500_diag_pattern writes it: 7 packets on PID 0x100 with
    running continuity counters, the number after the header, then the packet index."""
    out = b""
    for k in range(7):
        out += bytes([0x47, 0x01, 0x00, 0x10 | (7 * seq + k) & 15]) + seq.to_bytes(4, "big")
        out += bytes([k]) * 180
    return out


def main():
    c = udp_ts.Check(diag=True)
    for n, seq in enumerate([5, 6, 7, 9, 10, 10, 11]):    # 8 lost, the second 10 repeated
        c.add(diag(seq), 0.001 * n + (0.004 if seq == 9 else 0))
    s = c.summary()
    assert s["datagrams"] == 7 and s["packets"] == 49 and s["sizes"] == {"1316": 7}
    assert s["lost"] == 1 and s["reordered"] == 1 and (s["first_seq"], s["last_seq"]) == (5, 11), s
    # Datagram 8's seven packets are missing on PID 0x100 (an error at 9), and the repeated 10
    # starts its counters again (an error) but ends where the first 10 did, so 11 follows on.
    assert s["cc_errors"] == 2 and s["pids"]["0x0100"] == dict(packets=49, cc_errors=2), s
    assert s["max_gap_ms"] == 5.0 and s["sync_errors"] == s["tei"] == 0

    c = udp_ts.Check()
    bad = bytearray(diag(0))
    bad[188] = 0x46                                         # lost sync
    bad[376 + 1] |= 0x80                                    # the receiver's CRC-8 mark
    c.add(bytes(bad), 0.0)
    c.add(diag(1)[:376], 0.001)                              # a partial datagram is fine
    c.add(diag(2)[:200], 0.002)                              # a torn one is not
    null = bytes([0x47, 0x1F, 0xFF, 0x10]) + bytes(184)
    c.add(null * 3, 0.003)                                   # null packets: no continuity
    s = c.summary()
    assert s["sync_errors"] == 2 and s["tei"] == 1 and s["sizes"] == {"200": 1, "376": 1, "564": 1, "1316": 1}
    assert s["packets"] == 7 + 2 + 1 + 3 and "lost" not in s and "0x1fff" not in s["pids"], s

    # --pattern replay: byte for byte against the transmitter's 56-packet loop.
    loop = udp_ts.replay_packets()
    assert len(loop) == 56 and all(p[0] == 0x47 and len(p) == 188 for p in loop)
    good = sorted(loop)[:7]
    c = udp_ts.Check(packets=loop)
    c.add(b"".join(good), 0.0)
    filled = bytearray(b"".join(good))
    filled[2 * 188 + 6:] = b"\x47" * (len(filled) - 2 * 188 - 6)   # an underrun's fill, say
    c.add(bytes(filled), 0.001)
    s = c.summary()
    assert s["pattern_bad"] == 5 and s["bad_at"][0][:3] == [1, 2, 1316], s["bad_at"][:2]

    # Over a loopback socket, with the counters a run reports.
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for seq in range(20):
        tx.sendto(diag(seq), rx.getsockname())
    s = udp_ts.receive(rx, 5.0, 0.2, udp_ts.Check(diag=True)).summary()
    assert s["datagrams"] == 20 and s["lost"] == s["cc_errors"] == s["sync_errors"] == 0, s
    print("udp_ts checks passed")


if __name__ == "__main__":
    main()
