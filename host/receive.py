#!/usr/bin/env python3
"""Check a stopped S3 RX download and its board.py JSON-lines report."""
import argparse
import json
import pathlib
import struct
import zlib

PAIRS = 1600000
SENTINEL = 0xa5a5a5a5


def inspect_banks(data, report):
    """Describe a frozen geometry probe without treating it as a valid capture."""
    if len(data) != 3 * 65536 or report.get("probe", {}).get("bytes") != len(data):
        raise ValueError("probe must contain exactly three 64 KiB banks")
    if zlib.crc32(data) != report.get("crc32"):
        raise ValueError("raw bank CRC differs from the board")
    banks = []
    for n in range(3):
        words = struct.unpack("<16384I", data[n * 65536:(n + 1) * 65536])
        runs, start = [], None
        for at, word in enumerate((*words, SENTINEL)):
            if word != SENTINEL and start is None: start = at
            if word == SENTINEL and start is not None:
                runs.append([start, at]); start = None
        switch = report["probe"].get("before_switch", 0) & 16383
        windows = {"start": [(i & 16383, f"{words[i & 16383]:08x}") for i in range(-8, 9)],
                   "switch": [(i & 16383, f"{words[i & 16383]:08x}")
                              for i in range(switch - 8, switch + 9)]}
        banks.append({"address": hex(0x3fcb0000 + n * 65536),
                      "changed_words": sum(end - begin for begin, end in runs),
                      "changed_runs": len(runs), "runs_preview": runs[:16], "words": windows})
    return {"mode": "frozen geometry only; no continuity claim", "registers": report["probe"],
            "crc32": zlib.crc32(data), "banks": banks}


def validate(data, report, snapshot=None, *, pairs=PAIRS):
    if not 512 <= pairs <= 2000000:
        raise ValueError("expected pairs must be 512..2000000")
    if report.get("status") != "ESP_OK" or not report.get("complete"):
        raise ValueError("board did not complete the capture")
    if not report.get("guards_ok") or report.get("retained_pairs") != pairs:
        raise ValueError("board guards or retained sample count failed")
    if report.get("capture_path") == "packed" and not report.get("stage_guards_ok"):
        raise ValueError("packed staging guards failed")
    if len(data) != 2 * pairs or zlib.crc32(data) != report.get("crc32"):
        raise ValueError("download length or CRC differs from the board")
    for field in ("late", "overruns", "boundary_errors", "gap_events", "sentinel_errors"):
        if report.get(field) != 0:
            raise ValueError(f"board reported {field}={report.get(field)!r}")
    if report.get("dma_errors", 0):
        raise ValueError("board reported a DMA error")
    if report.get("cmd") == "pattern":
        for n in range(pairs):
            if data[2 * n] != ((13 * n) & 1023) >> 2 or \
               data[2 * n + 1] != ((29 * n + 7) & 1023) >> 2:
                raise ValueError(f"memory pattern differs at complex pair {n}")
    else:
        acquired, cycles = report.get("acquired_pairs", 0), report.get("engine_cycles", 0)
        if not pairs <= acquired < pairs + 12288 + 2000 or cycles <= 0:
            raise ValueError("acquisition count or engine timing is invalid")
        measured_rate = acquired * 240000000 / cycles
        if not 15840000 <= measured_rate <= 16160000:
            raise ValueError(f"index/cycle rate {measured_rate:.0f} differs from 16 MS/s")
        if snapshot is None or len(snapshot) != 2048:
            raise ValueError("capture needs the 512-word raw10 snapshot control")
        if zlib.crc32(snapshot) != report.get("snapshot_crc32"):
            raise ValueError("raw10 snapshot CRC differs from the board")
        for n, (word,) in enumerate(struct.iter_unpack("<I", snapshot)):
            if data[2 * n] != (word >> 12) & 255 or data[2 * n + 1] != (word >> 2) & 255:
                raise ValueError(f"raw10/sc8 conversion differs at complex pair {n}")
    result = {"pairs": pairs, "bytes": len(data), "crc32": zlib.crc32(data),
              "source": report["cmd"], "raw10_control_pairs": len(snapshot or b"") // 4}
    if report["cmd"] != "pattern": result["index_cycle_rate_sps"] = measured_rate
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iq", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    parser.add_argument("--snapshot", type=pathlib.Path)
    parser.add_argument("--pairs", type=int, default=PAIRS, help="expected complex samples in this capture build")
    parser.add_argument("--probe", action="store_true", help="inspect raw frozen banks")
    args = parser.parse_args()
    records = [json.loads(line) for line in args.report.read_text().splitlines() if line.strip()]
    kinds = ("probe",) if args.probe else ("capture", "pattern")
    report = next(r for r in reversed(records) if r.get("cmd") in kinds)
    snapshot = args.snapshot.read_bytes() if args.snapshot else None
    result = inspect_banks(args.iq.read_bytes(), report) if args.probe else \
        validate(args.iq.read_bytes(), report, snapshot, pairs=args.pairs)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
