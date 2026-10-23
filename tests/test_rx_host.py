"""host/rx.py: the short-frame payload model and the stream verdict."""
import pathlib
import sys

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

    wrong = frames(model[5:30], 100)
    wrong[3] = dict(wrong[3], payload=model[9])
    assert not rx.check(wrong, 7, "KJ5SCH", 40)["passed"]

    failed = frames(model[5:30], 100)
    failed[4] = dict(failed[4], length=-1, payload=b"")
    report = rx.check(failed, 7, "KJ5SCH", 40)
    assert not report["passed"] and report["failed"] == 1, report
    # CRC-32 records instead of payloads, at 5 Msym/s 8PSK.
    import zlib
    size16 = rx.frame_bytes(16)
    m16 = rx.model(size16, 30, "KJ5SCH")
    crcs = [dict(sequence=50 + k, length=size16, payload=zlib.crc32(p).to_bytes(4, "little"))
            for k, p in enumerate(m16[3:20])]
    report = rx.check(crcs, 16, "KJ5SCH", 30, 5e6)
    assert report["passed"] and report["compared"] == "crc32" and report["first_model_frame"] == 3, report
    assert abs(report["payload_mbps"] - 12.6407) < 1e-3, report
    crcs[5] = dict(crcs[5], payload=b"\0\0\0\0")
    assert not rx.check(crcs, 16, "KJ5SCH", 30, 5e6)["passed"]
    print("rx host checks passed")


if __name__ == "__main__":
    main()
