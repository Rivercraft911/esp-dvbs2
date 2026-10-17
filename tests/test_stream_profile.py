"""Cycle/cache interval arithmetic, including 32-bit wrap and 64-bit accumulation."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
from stream_profile import DEADLINE, MAX_FILL, MIN_SLACK, summarize

p = dict(calls=2, cycles=100, max_cycles=80, flash_miss=4, psram_miss=8,
         data_access=20, instruction_miss=6)
r = dict(cmd="dvbpace", seconds=60, status="ESP_OK", encoder_failed=False,
         late_fills=0, late_switches=0, fatal_late=0, underruns=0, max_fill_cycles=MAX_FILL,
         min_fill_slack_cycles=MIN_SLACK, encode_cycles_mean=350,
         profile=dict(fill_words=32760, fill_cycles=400, cache_scope="shared windows",
                      fill={k: dict(p) for k in ("copy", "whole", "partial")},
                      encoder={k: dict(p) for k in ("payload", "bb", "bch", "ldpc", "map", "pl")}))
s = summarize(r)
assert s["passes_60s_15_percent"]
assert s["fill_cycles_per_equivalent_bank"] == dict(copy=50, whole=50, partial=50, other=50)
assert s["encoder_kernel_cycles_per_frame"] == 250
assert s["cache_per_window"]["fill"]["copy"]["psram_miss"] == 4
for key, value in (("late_fills", 1), ("fatal_late", 1), ("underruns", 1),
                   ("min_fill_slack_cycles", MIN_SLACK - 1), ("max_fill_cycles", MAX_FILL + 1),
                   ("seconds", 59), ("status", "ESP_FAIL"), ("encoder_failed", True)):
    assert not summarize(dict(r, **{key: value}))["passes_60s_15_percent"], key
assert MAX_FILL == 72766 and MIN_SLACK == 12842 and DEADLINE == 85608
fast = dict(r, dac_rate=80000000, step_words=15740, max_fill_cycles=40137,
            min_fill_slack_cycles=7083)
assert summarize(fast)["passes_60s_15_percent"]
assert not summarize(dict(fast, max_fill_cycles=40138))["passes_60s_15_percent"]
assert not summarize(dict(fast, late_switches=1))["passes_60s_15_percent"]
with tempfile.TemporaryDirectory() as tmp:
    src = pathlib.Path(tmp) / "profile.c"
    src.write_text(r'''
#include <assert.h>
#include "stream_profile.h"
int main(void) {
    stream_profile_t p = {.counters = 1}, cycles_only = {0};
    stream_mark_t a = {UINT32_MAX - 9, UINT32_MAX, 50, 100, 70};
    stream_mark_t b = {10, 2, 55, 130, 70};
    stream_record(&p, a, b);
    assert(p.calls == 1 && p.cycles == 20 && p.max_cycles == 20);
    stream_record(&cycles_only, a, b);
    assert(cycles_only.cycles == 20 && cycles_only.flash_miss == 0 && cycles_only.data_access == 0);
    assert(p.flash_miss == 3 && p.psram_miss == 5 && p.data_access == 30);
    assert(p.instruction_miss == 0);
    for (int i = 0; i < 3; ++i)
        stream_record(&p, (stream_mark_t){0}, (stream_mark_t){.cycles = UINT32_MAX});
    assert(p.calls == 4 && p.cycles == 20 + 3ULL * UINT32_MAX);
    assert(p.max_cycles == UINT32_MAX);
}
''')
    exe = pathlib.Path(tmp) / "profile"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "firmware/main"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("stream profile: counter wrap, maxima and 64-bit totals pass")
