"""Saved VCM verification must reject corruption, gaps and same-length MODCOD swaps."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/"host"))
import check_vcm_dump as check
import decode


def rejects(fn, *args):
    try:
        fn(*args)
    except ValueError:
        return
    raise AssertionError("bad VCM evidence accepted")


def status(frames):
    return "FRAMELOCK 1\n" + "".join(
        f'S2FEC modcod={f["modcod"]} short=0 pilots=1 ldpc_flips=0 '
        'ldpc_checks_before=0 ldpc_checks_after=0 bch_errors=0\n' for f in frames)


def test():
    expected = check.expected_frames([4, 11, 13, 17], 12, 1, "N0CALL")
    for start in (0, 1, 2):
        frames = expected[start:start+10]
        raw = b"".join(f["bbf"] for f in frames)
        report, manifest, payload = check.verify(raw, status(frames), expected)
        assert report["first_frame"] == start and report["last_frame"] == start+9
        assert [row["frame"] for row in manifest] == list(range(start, start+10))
        assert payload == b"".join(f["payload"] for f in frames)
    frames = expected[:10]
    raw, log = b"".join(f["bbf"] for f in frames), status(frames)
    # Payload corruption; CRC-valid changes to CCM; equal-size QPSK/8PSK PLS labels.
    corrupt = bytearray(raw); corrupt[50] ^= 1
    rejects(check.verify, bytes(corrupt), log, expected)
    corrupt = bytearray(raw); corrupt[0] ^= 16; corrupt[9] = decode.crc8(corrupt[:9])
    rejects(check.verify, bytes(corrupt), log, expected)
    rejects(check.verify, raw, log.replace("modcod=11", "modcod=17", 1), expected)
    rejects(check.verify, raw, log.replace("bch_errors=0", "bch_errors=-1", 1), expected)
    rejects(check.verify, raw, log.replace("ldpc_flips=0", "ldpc_flips=1", 1), expected)
    rejects(check.verify, raw, log.replace("pilots=1", "pilots=0", 1), expected)
    rejects(check.verify, raw, "FRAMELOCK 1\n", expected)
    # Losing or repeating a frame can retain all MODCODs and byte counts.
    for broken in (expected[:4]+expected[5:], expected[:4]+expected[3:10], expected[:8]):
        rejects(check.verify, b"".join(f["bbf"] for f in broken), status(broken), expected)
    assert [len(f["payload"]) for f in expected[:4]] == [4016, 7264, 5370, 7264]
    print("VCM dump: unique contiguous full BBFRAME/model match; corruption, gaps, duplicates, CCM and PLS swaps rejected")


if __name__ == "__main__":
    test()
