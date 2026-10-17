"""Check fecbench source/CRCs natively, or verify coordinator JSONL from the board.

    python3 tests/test_fecbench.py
    python3 tests/test_fecbench.py results/<date>-fec/runs.jsonl [--correctness-only]

fecbench runs one implementation per command: plain (dvbs2.c), c (fast.c in
portable C) or pie (fast.c with the ESP32-S3 vector kernels). All three must
produce the same 140 phase CRCs.

The device recipe is fixed in firmware/main/fecbench.c. Timing is never inferred
from the native test: its clock advances a synthetic 17 counts per read. Native
records stay in memory and never enter the performance report. Board records
must contain complete 140-frame runs.
"""
import json
import os
import pathlib
import shlex
import statistics
import subprocess
import sys
import tempfile
import zlib

import numpy as np

from test_dvbs2_c import MODES, ROOT, d

FRAMES = 5
STAGES = ("bb", "bb_scramble", "bch", "ldpc", "interleave_map", "pl", "total")
IMPLS = ("plain", "c", "pie")
TARGET = 250_000   # brief: one 240 MHz core per normal 8PSK 2/3 frame at 20 Msym/s


def source(seed, count):
    out, x = np.empty(count, np.uint8), seed
    for k in range(count):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[k] = x & 255
    return out


def reference_records():
    expected = {}
    for modcod, modulation, rate in MODES:
        c = d.code(rate)
        size = (c.kbch - 80) // 8
        for input_id, input_name in enumerate(("gcs", "ts")):
            seed = 2026 ^ (modcod << 16) ^ (input_id << 8)
            count = ((FRAMES * size + 187) // 188) * 188
            stream = source(seed, count)
            if input_id:
                stream[::188] = 0x47
                bbframes = d.ts_bbframes(stream.reshape(-1, 188), rate)[:FRAMES]
            else:
                bbframes = [d.bbframe(np.unpackbits(stream[k * size:(k + 1) * size]), rate)
                            for k in range(FRAMES)]
            for frame, bb in enumerate(bbframes):
                fec = d.fecframe(bb, rate)
                phases = d.plframe_phases(fec, rate, modulation=modulation)
                crc = zlib.crc32(phases.tobytes())
                key = modcod, input_name, frame
                expected[key] = dict(rate=rate, modulation=modulation, seed=seed,
                                     source_offset=frame * size, payload_bytes=size,
                                     symbols=len(phases), crc32=crc)
    assert len(expected) == 140
    return expected


def native_records(impl="plain"):
    """Run the actual command with synchronous task stubs and a synthetic clock.

    This checks the firmware's source generator, TS continuation, phase CRC and
    JSON protocol together. It measures no timing and accesses no hardware.
    """
    with tempfile.TemporaryDirectory(prefix="fecbench-native-") as tmp:
        folder = pathlib.Path(tmp)
        (folder / "freertos").mkdir()
        (folder / "native.h").write_text(r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define tskIDLE_PRIORITY 0
typedef void *TaskHandle_t;
typedef struct { const char *version, *idf_ver; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = {"native-synthetic-cycles", "native-stubs"};
    return &desc;
}
static inline uint32_t esp_cpu_get_cycle_count(void)
{
    static uint32_t count;
    return count += 17; /* Synthetic values test the schema, never performance. */
}
static inline int64_t esp_timer_get_time(void) { return 1; }
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, unsigned caps)
{
    (void)align; (void)caps;
    return malloc(size);
}
static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}
#define heap_caps_free free
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 65536; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 32768; }
static inline int xPortGetCoreID(void) { return 1; }
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
static inline void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
static inline void vTaskDelete(TaskHandle_t task) { (void)task; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
static inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                        unsigned stack, void *arg, unsigned priority,
                                        TaskHandle_t *handle, int core)
{
    (void)name; (void)stack; (void)priority; (void)handle; (void)core;
    fn(arg);
    return pdPASS;
}
static inline unsigned ulTaskNotifyTake(int clear, unsigned timeout)
{
    (void)clear; (void)timeout;
    return 1;
}
''')
        for header in ("esp_app_desc.h", "esp_cpu.h", "esp_heap_caps.h", "esp_timer.h",
                       "sdkconfig.h", "freertos/FreeRTOS.h", "freertos/task.h"):
            (folder / header).write_text('#include "native.h"\n')
        main = folder / "main.c"
        main.write_text('#include "fecbench.h"\n'
                        'int main(int argc, char **argv) { (void)argc; '
                        'return fecbench_run(argv[1]) ? 1 : 0; }\n')
        executable = folder / "fecbench"
        component = ROOT / "firmware/components/dvbs2"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(folder), "-I", str(ROOT / "firmware/main"),
            "-I", str(component / "include"), str(main),
            str(ROOT / "firmware/main/fecbench.c"), str(component / "dvbs2.c"),
            str(component / "fast.c"), "-o", str(executable)], check=True)
        output = subprocess.run([str(executable), impl], check=True, capture_output=True,
                                text=True).stdout
        lines = output.splitlines()
        assert all(line.startswith("@") for line in lines), output
        return [json.loads(line[1:]) for line in lines]


def check_records(records, expected):
    """Reject incomplete/duplicate/inconsistent runs before interpreting timing."""
    runs, active = [], None
    for record in records:
        if record.get("cmd") != "fecbench":
            continue
        assert "error" not in record, record
        event = record.get("event")
        if event == "begin":
            assert active is None, "new run before previous end"
            for key, value in dict(cpu_mhz=240, core=1, modes=14, frames_per_mode=5,
                                   warmup_frames=1, pilots=True, pl_code=0, rolloff=0.35,
                                   inputs=["gcs", "ts"]).items():
                assert record[key] == value, (key, record.get(key))
            assert record["firmware"] and record["idf"]
            impl = record.get("impl", "plain")   # records before exp/fec-limit had no impl
            assert impl in IMPLS, impl
            if "bch_variant" in record:
                assert record["bch_variant"] in (("pie", "legacy", "nibble", "sram")
                                                  if impl == "pie" else (impl,)), record
            if impl == "pie":
                assert record["pie_mask"] == 15, ("PIE kernels not all in use", record)
            active = dict(metadata=record, impl=impl, frames={})
        elif event == "mode":
            assert active is not None, "mode without begin"
            assert record["modcod"] in {k[0] for k in expected}
            assert record["output_memory"] in {"internal", "psram"}
            memory = active.setdefault("memory", {})
            assert record["modcod"] not in memory, "duplicate mode"
            for key in ("fast_bytes", "free_internal", "largest_internal"):
                assert type(record[key]) is int and record[key] >= 0
            memory[record["modcod"]] = record
        elif event == "frame":
            assert active is not None, "frame without begin"
            key = record["modcod"], record["input"], record["frame"]
            assert key in expected and key not in active["frames"], ("unknown/duplicate", key)
            for field, value in expected[key].items():
                assert record[field] == value, (key, field, record.get(field), value)
            assert record.get("impl", "plain") == active["impl"], key
            cycles = record["cycles"]
            assert set(cycles) == set(STAGES), (key, cycles)
            # The fast implementations scramble inside bb, so bb_scramble is 0 there.
            zero = {"bb_scramble"} if active["impl"] != "plain" else set()
            for stage, value in cycles.items():
                assert type(value) is int and 0 <= value < 2**32, (key, stage)
                assert (value == 0) == (stage in zero), (key, stage, value)
            assert cycles["total"] >= sum(cycles[s] for s in STAGES[:-1]), key
            active["frames"][key] = record
        elif event == "pie_check":
            assert active is not None and active["impl"] == "pie", "pie_check outside a pie run"
            assert record["mismatch"] == 0, ("PIE stage differs from C (1 bb, 2 bch, 4 ldpc, "
                                             "8 map+pl, 16 packed 8PSK)", record)
            checked = active.setdefault("checked", set())
            assert record["modcod"] not in checked, "duplicate pie_check"
            checked.add(record["modcod"])
        elif event == "end":
            assert active is not None, "end without begin"
            assert record["frames_reported"] == len(expected)
            assert set(active["frames"]) == set(expected), "missing frames"
            if "workspace_memory" in active["metadata"]:
                assert set(active.get("memory", {})) == {k[0] for k in expected}, "mode memory report missing"
            if active["impl"] == "pie":
                assert active.get("checked") == {k[0] for k in expected}, "pie_check missing"
            runs.append(active)
            active = None
        else:
            assert record.get("done"), ("unknown record", record)
    assert active is None, "incomplete last run"
    assert runs, "no fecbench runs"
    return runs


def test_checker(expected):
    # Synthetic cycle values exercise record validation only. They are never
    # saved under results/ or presented as performance measurements.
    begin = dict(cmd="fecbench", event="begin", cpu_mhz=240, core=1, modes=14,
                 frames_per_mode=5, warmup_frames=1, pilots=True, pl_code=0,
                 rolloff=0.35, inputs=["gcs", "ts"], firmware="test", idf="test")
    frames = [dict(cmd="fecbench", event="frame", modcod=k[0], input=k[1], frame=k[2],
                   cycles={s: 10 if s == "total" else 1 for s in STAGES}, **v)
              for k, v in expected.items()]
    end = dict(cmd="fecbench", event="end", frames_reported=140)
    good = [begin, *frames, end]
    assert len(check_records(good * 2, expected)) == 2
    for variant in ("pie", "legacy", "nibble", "sram"):
        pie_begin = dict(begin, impl="pie", pie_mask=15, bch_variant=variant)
        pie_frames = [dict(row, impl="pie", cycles=dict(row["cycles"], bb_scramble=0))
                      for row in frames]
        checks = [dict(cmd="fecbench", event="pie_check", modcod=m, mismatch=0)
                  for m, _, _ in MODES]
        pie_run = [pie_begin, *checks, *pie_frames, end]
        assert check_records(pie_run, expected)[0]["metadata"]["bch_variant"] == variant
        for broken in ([dict(pie_begin, bch_variant="unknown"), *pie_run[1:]],
                       [*pie_run[:-1], checks[0], end]):
            try:
                check_records(broken, expected)
            except AssertionError:
                pass
            else:
                raise AssertionError("checker accepted an invalid variant or duplicate check")
    bad_crc = dict(frames[0], crc32=frames[0]["crc32"] ^ 1)
    bad_cycles = dict(frames[0], cycles=dict(frames[0]["cycles"], total=1))
    for bad in ([dict(begin, workspace_memory="psram"), *frames, end],
                good[:-1], [begin, *frames[1:], end],
                [begin, *frames, frames[0], end], [begin, bad_crc, *frames[1:], end],
                [begin, bad_cycles, *frames[1:], end]):
        try:
            check_records(bad, expected)
        except AssertionError:
            pass
        else:
            raise AssertionError("checker accepted a corrupt/incomplete run")


def report(runs):
    """Print each run; return whether every c and pie run meets its budget.

    Budget for 8PSK 2/3 (GCS and TS, every sampled frame): one core at 5 Msym/s
    (1065312 cycles) for c and pie, and TARGET (one core at 20 Msym/s) for pie.
    plain runs are the known-slow reference and only reported."""
    passed = True
    for index, run in enumerate(runs, 1):
        memory = run.get("memory", {}).get(13)
        if memory and run["impl"] != "plain":
            fits = memory["free_internal"] >= 32768 and memory["largest_internal"] >= 16384
            print(f"8PSK 2/3 heap: {memory['free_internal']} bytes free, "
                  f"{memory['largest_internal']} largest: {'PASS' if fits else 'FAIL'}")
            passed &= fits
        print(f"run {index}: all 140 CRCs match; firmware {run['metadata']['firmware']}, "
              f"impl {run['impl']}, BCH {run['metadata'].get('bch_variant', 'original')}")
        worst = 0
        for modcod, modulation, rate in MODES:
            for input_name in ("gcs", "ts"):
                rows = [run["frames"][(modcod, input_name, k)] for k in range(FRAMES)]
                totals = [r["cycles"]["total"] for r in rows]
                rs = 5e6 if modulation == "8psk" else 1e6
                budget = int(240e6 * rows[0]["symbols"] / rs)
                if modcod == 13:
                    worst = max(worst, max(totals))
                print(f"  {modulation} {rate:>4} {input_name}: cycles min/median/max "
                      f"{min(totals)}/{statistics.median(totals):.0f}/{max(totals)}, "
                      f"max {100 * max(totals) / budget:.1f}% of {budget} cycles "
                      f"at {rs / 1e6:g} Msym/s")
                print("    stage medians: " + ", ".join(
                    f"{s}={statistics.median(r['cycles'][s] for r in rows):.0f}"
                    for s in STAGES[:-1]))
        limit = {"plain": None, "c": 1065312, "pie": TARGET}[run["impl"]]
        verdict = "reference" if limit is None else ("PASS" if worst < limit else "FAIL")
        if limit is not None:
            passed &= worst < limit
        print(f"  {verdict}: slowest 8PSK 2/3 frame {worst} cycles"
              + ("" if limit is None else f", limit {limit}"))
    return passed


if __name__ == "__main__":
    if len(sys.argv) == 1:
        expected = reference_records()
        for impl in ("plain", "c"):
            runs = check_records(native_records(impl), expected)
            assert len(runs) == 1 and runs[0]["impl"] == impl
        test_checker(expected)
        print("fecbench: actual C command (plain and fast C) emits 140 phase CRCs equal to the "
              "reference; checker rejects missing, duplicate and corrupt records. "
              "Native clock counts are synthetic; no performance was measured.")
    else:
        correctness_only = "--correctness-only" in sys.argv[1:]
        paths = [arg for arg in sys.argv[1:] if arg != "--correctness-only"]
        assert len(paths) == 1, __doc__
        records = [json.loads(line.removeprefix("@"))
                   for line in pathlib.Path(paths[0]).read_text().splitlines() if line.strip()]
        runs = check_records(records, reference_records())
        if correctness_only:
            for run in runs:
                print(f"{run['metadata'].get('bch_variant', run['impl'])}: 140 CRCs match, "
                      f"{len(run.get('checked', []))} PIE checks; timing not judged")
            raise SystemExit(0)
        ok = report(runs)
        if any(run["impl"] != "plain" for run in runs):
            print(("PASS" if ok else "FAIL") + ": every c run < 1065312 cycles (8PSK 2/3 at "
                  f"5 Msym/s) and every pie run < {TARGET} (20 Msym/s), slowest sampled frame")
        else:
            print("plain runs only: correctness checked, no budget judged")
        sys.exit(0 if ok else 1)
