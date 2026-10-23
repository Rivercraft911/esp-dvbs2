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
    print("rx host checks passed")


if __name__ == "__main__":
    main()
