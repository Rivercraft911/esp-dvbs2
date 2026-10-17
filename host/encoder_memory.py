"""Inventory the 8PSK stream's hot cache lines from a linked ELF, not table declarations.

The set model assumes index = (virtual address / line bytes) % sets. It is a
placement sensitivity model, not a cache simulator or measured stall count.
The union excludes interrupts, other tasks, small scalar constants and stacks.
"""
import argparse
import json
import subprocess

from dvbs2_tables import BCH_PARAMS
from gen_dvbs2_c import RATES


def symbols(text):
    out = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 4:
            out[fields[3]] = int(fields[0], 16), int(fields[1], 16)
    return out


def inventory(syms, modcod, workspace=None, resident_bch=False, line_bytes=32):
    rate = {13: "2/3", 15: "5/6"}[modcod]
    kbch = BCH_PARAMS["normal"][rate][0] // 8
    direct = "fast_map_3_packed" in syms
    stages = {k: set() for k in ("bb", "bch", "ldpc", "map", "pl")}
    tables = []

    def span(stage, name, offset=0, length=None, offsets=None):
        address, size = syms[name]
        if length is None:
            length = size - offset
        assert 0 <= offset <= size and offset + length <= size
        if offsets is None:
            touched = set(range((address + offset) // line_bytes,
                                (address + offset + length + line_bytes - 1) // line_bytes))
        else:
            touched = {(address + at) // line_bytes for at in offsets}
        stages[stage].update(touched)
        tables.append(dict(stage=stage, name=name, address=hex(address + offset),
                           cache_bytes=len(touched) * line_bytes))

    span("bb", "bb_prbs", length=kbch)
    span("bb", "fast_crc8", length=256)       # conservative CRC index coverage
    if not resident_bch:
        span("bch", "fast_bch_10")
    span("ldpc", f"fast_prog_{RATES.index(rate)}")
    span("ldpc", "fast_cols")
    span("map", "fast_gold_3_1")
    if direct:
        span("map", "fast_map_3_packed")
    else:
        # Only 64 words of the old sparse 4 KiB table are addressed. At 32 B
        # per line they occupy 512 B, not the full allocation's 4096 B.
        span("map", "fast_map_3", offsets=[4 * i for i in range(1024) if not i & ~0x333])
        span("pl", "fast_headers", offset=90 * (2 * (modcod - 4) + 1), length=90)
        span("pl", "fast_pilots", length=14 * 36)
    flash = set().union(*stages.values())
    if workspace is not None:
        for stage, n in (("bb", kbch), ("bch", kbch + 20), ("ldpc", 8112), ("map", 8112)):
            stages[stage].update(range(workspace // line_bytes,
                                      (workspace + n + line_bytes - 1) // line_bytes))
    union = set().union(*stages.values())
    sets = 32768 // (8 * line_bytes)
    occupancy = [0] * sets
    for line in union:
        occupancy[line % sets] += 1
    return dict(modcod=modcod, direct_packed=direct, tables=tables,
                workspace="internal" if workspace is None else hex(workspace),
                resident_bch=resident_bch, flash_cache_bytes=len(flash) * line_bytes,
                union_cache_bytes=len(union) * line_bytes,
                stage_cache_bytes={k: len(v) * line_bytes for k, v in stages.items()},
                set_model=dict(cache_bytes=32768, ways=8, line_bytes=line_bytes,
                               max_lines=max(occupancy), sets_over_8=sum(v > 8 for v in occupancy)))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("elf")
    p.add_argument("--nm", default="xtensa-esp32s3-elf-nm")
    p.add_argument("--workspace", type=lambda v: int(v, 0),
                   help="PSRAM workspace address from a board layout record; omit for internal")
    p.add_argument("--resident-bch", action="store_true")
    args = p.parse_args()
    syms = symbols(subprocess.check_output([args.nm, "-S", args.elf], text=True))
    for mc in (13, 15):
        print(json.dumps(inventory(syms, mc, args.workspace, args.resident_bch)))


if __name__ == "__main__":
    main()
