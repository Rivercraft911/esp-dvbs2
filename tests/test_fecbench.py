"""Check fecbench source/CRCs natively, or verify coordinator JSONL from the board.

    python3 tests/test_fecbench.py
    python3 tests/test_fecbench.py results/<date>-fec/runs.jsonl

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


def native_records():
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
        main.write_text('#include "fecbench.h"\nint main(void) { fecbench_run(); return 0; }\n')
        executable = folder / "fecbench"
        component = ROOT / "firmware/components/dvbs2"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(folder), "-I", str(ROOT / "firmware/main"),
            "-I", str(component / "include"), str(main),
            str(ROOT / "firmware/main/fecbench.c"), str(component / "dvbs2.c"),
            "-o", str(executable)], check=True)
        output = subprocess.run([str(executable)], check=True, capture_output=True,
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
            active = dict(metadata=record, frames={})
        elif event == "frame":
            assert active is not None, "frame without begin"
            key = record["modcod"], record["input"], record["frame"]
            assert key in expected and key not in active["frames"], ("unknown/duplicate", key)
            for field, value in expected[key].items():
                assert record[field] == value, (key, field, record.get(field), value)
            cycles = record["cycles"]
            assert set(cycles) == set(STAGES), (key, cycles)
            assert all(type(v) is int and 0 < v < 2**32 for v in cycles.values()), key
            assert cycles["total"] >= sum(cycles[s] for s in STAGES[:-1]), key
            active["frames"][key] = record
        elif event == "end":
            assert active is not None, "end without begin"
            assert record["frames_reported"] == len(expected)
            assert set(active["frames"]) == set(expected), "missing frames"
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
    bad_crc = dict(frames[0], crc32=frames[0]["crc32"] ^ 1)
    bad_cycles = dict(frames[0], cycles=dict(frames[0]["cycles"], total=1))
    for bad in (good[:-1], [begin, *frames[1:], end],
                [begin, *frames, frames[0], end], [begin, bad_crc, *frames[1:], end],
                [begin, bad_cycles, *frames[1:], end]):
        try:
            check_records(bad, expected)
        except AssertionError:
            pass
        else:
            raise AssertionError("checker accepted a corrupt/incomplete run")


def report(runs):
    target_pass = True
    for index, run in enumerate(runs, 1):
        print(f"run {index}: all 140 CRCs match; firmware {run['metadata']['firmware']}")
        for modcod, modulation, rate in MODES:
            for input_name in ("gcs", "ts"):
                rows = [run["frames"][(modcod, input_name, k)] for k in range(FRAMES)]
                totals = [r["cycles"]["total"] for r in rows]
                rs = 5e6 if modulation == "8psk" else 1e6
                budget = int(240e6 * rows[0]["symbols"] / rs)
                fit = max(totals) < budget
                if modcod == 13:
                    target_pass &= fit
                print(f"  {modulation} {rate:>4} {input_name}: cycles min/median/max "
                      f"{min(totals)}/{statistics.median(totals):.0f}/{max(totals)}, "
                      f"max {100 * max(totals) / budget:.1f}% of {budget} cycles "
                      f"at {rs / 1e6:g} Msym/s")
                print("    stage medians: " + ", ".join(
                    f"{s}={statistics.median(r['cycles'][s] for r in rows):.0f}"
                    for s in STAGES[:-1]))
    print(("PASS" if target_pass else "FAIL") +
          " encoder budget: every sampled 8PSK 2/3 GCS and TS frame < 1065312 cycles")
    return target_pass


if __name__ == "__main__":
    if len(sys.argv) == 1:
        expected = reference_records()
        assert len(check_records(native_records(), expected)) == 1
        test_checker(expected)
        print("fecbench: actual C command emits 140 phase CRCs equal to the reference; "
              "checker rejects missing, duplicate and corrupt records. "
              "Native clock counts are synthetic; no performance was measured.")
    else:
        assert len(sys.argv) == 2, __doc__
        records = [json.loads(line.removeprefix("@"))
                   for line in pathlib.Path(sys.argv[1]).read_text().splitlines() if line.strip()]
        runs = check_records(records, reference_records())
        sys.exit(0 if report(runs) else 1)
