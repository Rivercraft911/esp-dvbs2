"""Cache footprint uses touched lines, including sparse tables and shared lines."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
from encoder_memory import inventory, symbols


def test_inventory():
    syms = {}
    sizes = dict(bb_prbs=7274, fast_crc8=1024, fast_bch_10=4096,
                 fast_prog_2=1936, fast_prog_5=1730, fast_cols=720,
                 fast_gold_3_1=5440, fast_map_3=4096, fast_headers=2520, fast_pilots=792)
    for i, (name, size) in enumerate(sizes.items()):
        syms[name] = 0x3c000000 + i * 0x10000, size
    text = "\n".join(f"{a:08x} {n:08x} r {name}" for name, (a, n) in syms.items())
    assert symbols(text) == syms
    old = inventory(syms, 13, 0x3d000010)
    rows = {r["name"]: r for r in old["tables"]}
    assert rows["fast_map_3"]["cache_bytes"] == 512
    assert rows["bb_prbs"]["cache_bytes"] == 5408
    assert old["union_cache_bytes"] == old["flash_cache_bytes"] + 8128
    syms["fast_map_3_packed"] = 0x3c100000, 8192
    new = inventory(syms, 15, resident_bch=True)
    names = {r["name"] for r in new["tables"]}
    assert "fast_bch_10" not in names and "fast_headers" not in names
    assert new["stage_cache_bytes"]["pl"] == 0
    assert new["stage_cache_bytes"]["map"] == 8192 + 5440
    assert new["union_cache_bytes"] == new["flash_cache_bytes"]
    sparse = inventory(syms, 15, 0x3d000010, direct_packed=False)
    direct = inventory(syms, 15, 0x3d000010)
    assert not sparse["direct_packed"]
    assert direct["union_cache_bytes"] - sparse["union_cache_bytes"] == 8192 - 512
    assert sparse["stage_cache_bytes"]["pl"] == 0
    assert sparse["stage_cache_bytes"]["map"] == 512 + 5440 + 8128
    # Overlapping tables must count a physical line once.
    syms["fast_crc8"] = syms["bb_prbs"]
    shared = inventory(syms, 15, resident_bch=True)
    assert shared["flash_cache_bytes"] == new["flash_cache_bytes"] - 256


if __name__ == "__main__":
    test_inventory()
    print("encoder memory: sparse/full tables, PSRAM over-read lines, resident tables and line unions pass")
