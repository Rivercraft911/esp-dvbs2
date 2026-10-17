"""Summarize radio-off stream profiles from board.py JSONL, without accessing a device.

Fill phases are normalized by words to one 16,380-word bank. Maxima of different
phases must not be added. Cache windows include both cores and overlap in time.
"""
import argparse
import json
import pathlib

BANK = 16380
DEADLINE = 85608
MAX_FILL = DEADLINE * 85 // 100
MIN_SLACK = (DEADLINE * 15 + 99) // 100


def summarize(r):
    p = r["profile"]
    fill = {name: stage["cycles"] * BANK / p["fill_words"]
            for name, stage in p["fill"].items()} if p["fill_words"] else {}
    if fill:
        fill["other"] = p["fill_cycles"] * BANK / p["fill_words"] - sum(fill.values())
    encoder = {name: stage["cycles"] / stage["calls"] if stage["calls"] else 0
               for name, stage in p["encoder"].items()}
    out = dict(command=r.get("command", r["cmd"]),
               fill_cycles_per_equivalent_bank=fill, encoder_cycles_per_frame=encoder,
               encoder_kernel_cycles_per_frame=sum(v for k, v in encoder.items() if k != "payload"),
               encode_cycles_mean=r["encode_cycles_mean"],
               cache_scope=p["cache_scope"],
               cache_per_window={side: {name: {key: stage[key] / stage["calls"] if stage["calls"] else 0
                                              for key in ("flash_miss", "psram_miss", "data_access", "instruction_miss")}
                                       for name, stage in stages.items()}
                                 for side, stages in (("fill", p["fill"]), ("encoder", p["encoder"]))})
    if r["cmd"] == "dvbpace":
        out["passes_60s_15_percent"] = (r.get("seconds", 0) >= 60 and r.get("status") == "ESP_OK"
            and not r.get("encoder_failed", True)
            and all(r.get(k, 1) == 0 for k in ("late_fills", "fatal_late", "underruns"))
            and r.get("max_fill_cycles", DEADLINE) <= MAX_FILL
            and r.get("min_fill_slack_cycles", 0) >= MIN_SLACK)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("records", type=pathlib.Path, nargs="+")
    args = parser.parse_args()
    for file in args.records:
        for line in file.read_text().splitlines():
            r = json.loads(line)
            if r.get("cmd") in ("dvbpace", "dvbdump") and "profile" in r:
                print(json.dumps(summarize(r), sort_keys=True))


if __name__ == "__main__":
    main()
