"""host/rx.py: the short-frame payload model and the stream verdict."""
import pathlib
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "host"))
import decode  # noqa: E402
import rx  # noqa: E402


def frames(payloads, first_sequence=0):
    return [dict(sequence=first_sequence + k, length=len(p), payload=p) for k, p in enumerate(payloads)]


def main():
    assert [rx.frame_bytes(mc) for mc in (4, 6, 7, 10, 13, 16)] == [869, 1319, 1454, 1769, 1319, 1769]
    assert [rx.frame_symbols(mc) for mc in (7, 16)] == [8370, 5598]
    size = rx.frame_bytes(7)
    model = rx.model(size, 40, "KJ5SCH")
    # Same PRNG and header as the normal-frame model, cut at short-frame boundaries.
    stream = b"".join(p for _, p in decode.payload_sequence((7,), 2, "KJ5SCH"))
    assert model[0] == stream[:size]

    ok = rx.check(frames(model[5:30], 100), 7, "KJ5SCH", 40)
    assert ok["passed"] and ok["first_model_frame"] == 5 and ok["exact"] == 25, ok

    lost = frames(model[5:30], 100)
    del lost[10]
    bad = rx.check(lost, 7, "KJ5SCH", 40)
    assert not bad["passed"] and bad["missing"] == 1, bad
    assert bad["missing_runs"] == [[110, 1, 0.084]], bad["missing_runs"]   # 10 frames of 8.37 ms in

    wrong = frames(model[5:30], 100)
    wrong[3] = dict(wrong[3], payload=model[9])
    assert not rx.check(wrong, 7, "KJ5SCH", 40)["passed"]

    failed = frames(model[5:30], 100)
    failed[4] = dict(failed[4], length=-1, payload=b"")
    report = rx.check(failed, 7, "KJ5SCH", 40)
    assert not report["passed"] and report["failed"] == 1, report
    # 5 Msym/s records carry the payload's CRC-32: 8PSK 8/9 short frames, 12.64 Mb/s of payload.
    size = rx.frame_bytes(16)
    model = rx.model(size, 40, "KJ5SCH")
    crcs = [dict(sequence=7 + k, length=size, payload=b"", crc=zlib.crc32(p)) for k, p in enumerate(model[3:30])]
    ok = rx.check(crcs, 16, "KJ5SCH", 40, 5e6)
    assert ok["passed"] and ok["first_model_frame"] == 3 and ok["payload_mbps"] == 12.6402, ok
    crcs[5] = dict(crcs[5], crc=crcs[5]["crc"] ^ 1)
    assert not rx.check(crcs, 16, "KJ5SCH", 40, 5e6)["passed"]

    # TS input: the model's data fields are host/dvbs2.py ts_bbframes of the transmitter's
    # replay loop and of a live sender's counting pattern (60 frames cross the 56-packet loop).
    import numpy as np
    import dvbs2
    for modcod, rate, replay in ((7, "3/4", True), (16, "8/9", False), (13, "2/3", True)):
        stream = b"".join(rx.ts_messages(0, 120, replay))
        want = [np.packbits(f[80:]).tobytes() for f in
                dvbs2.ts_bbframes(np.frombuffer(stream, np.uint8), rate, short=True)]
        assert rx.ts_model(modcod, 60, replay) == want[:60], (modcod, replay)
    assert rx.ts_messages(8, 1, True) == rx.ts_messages(0, 1, True) != rx.ts_messages(8, 1, False)
    model = rx.ts_model(7, 40)
    crcs = [dict(sequence=3 + k, length=len(p), payload=b"", crc=zlib.crc32(p)) for k, p in enumerate(model[5:35])]
    ok = rx.check(crcs, 7, "KJ5SCH", 40, 5e6, payloads=model)
    assert ok["passed"] and ok["first_model_frame"] == 5 and ok["payload_mbps"] == 6.9486, ok

    # The TS counters must account for every exact frame: 30 frames from model frame 5 hold
    # packets 39 (the first to start in frame 5, 7,270 bytes in) to 269, all checked.
    # A deframer that stopped early, or took fewer frames, fails although every frame is exact.
    assert rx.ts_packets_expected(5, 30, 1454) == (269 - 39 + 1, 0)
    assert rx.ts_packets_expected(0, 3, 188 * 5) == (15, 1)        # ends on a packet boundary
    assert rx.ts_packets_expected(1, 1, 100) == (0, 0)             # no packet both starts and ends
    good = dict(decoded=0, ts=dict(frames=30, packets=231, crc_errors=0, unchecked=0, null_packets=0,
                                   cc_errors=0, resyncs=0, late=0, not_ts=0,
                                   pattern=dict(checked=231, bad=0, jumps=0, wraps=0)))
    assert rx.ts_verdict([good], False, ok)["passed"]
    for taken, packets in ((1, 1), (30, 230), (29, 231), (30, 232)):
        bad = dict(good, ts=dict(good["ts"], frames=taken, packets=packets,
                                 pattern=dict(good["ts"]["pattern"], checked=packets)))
        assert not rx.ts_verdict([bad], False, ok)["passed"], (taken, packets)
    # Long runs (540 s): the stream comes from row jumps, not one step at a time, and must equal
    # xorshift32 stepped byte by byte; CRC keys stand for the payloads; the model covers the run.
    x, ref = 2026, bytearray()
    for _ in range(9000):
        x = rx._step(x)
        ref.append(x & 255)
    assert rx.stream(9000) == bytes(ref) and rx.model(1454, 3, "KJ5SCH")[2][40:] == ref[2948:4362]
    assert rx.model_keys(1769, 20, "KJ5SCH", True) == [zlib.crc32(p) for p in rx.model(1769, 20, "KJ5SCH")]
    assert rx.model_frames(530, "5m", 16) * 5598 / 5e6 > 590 and rx.model_frames(530, "1m", 7) * 8370 / 1e6 > 590
    # The replay's fields repeat with its 56-packet loop, apart from frame 0: ts_keys equals ts_model.
    import math
    for modcod in (7, 16):
        n = 2 * 10528 // math.gcd(rx.frame_bytes(modcod), 10528) + 3
        fields = rx.ts_model(modcod, n, True)
        assert list(rx.ts_keys(modcod, n, True, False)) == fields
        assert list(rx.ts_keys(modcod, n, True, True)) == [zlib.crc32(f) for f in fields]
    print("rx host checks passed")


if __name__ == "__main__":
    main()
