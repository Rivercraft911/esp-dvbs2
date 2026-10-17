"""Compile the actual console parsers against counted stubs. No board is used.

tx and dvb key the radio; dvbdump and dvbpace must not. Every invalid request must stop in the
parser, and every valid one must reach exactly its own function with the parsed values.
"""
import json
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Extract just the parser functions so this safety check needs no simulated
# RTOS, USB or Wi-Fi stack. Every tested branch is compiled from main.c itself.
source = (ROOT / "firmware/main/main.c").read_text()
parser = source[source.index("static bool number("):source.index("static void cmd_info(")]
invalid = [
    "", "2400", "2400 tone", "2400 tone 1 0 extra", "2400 unknown 1 0",
    "2389 tone 1 0", "2451 tone 1 0", "-2400 tone 1 0", "nan tone 1 0",
    "2400.0 tone 1 0", "2.4e3 tone 1 0", "0x960 tone 1 0",
    "999999999999999999999999999999 tone 1 0", "2400 tone 0 0",
    "2400 tone 61 0", "2400 tone -1 0", "2400 tone 1.5 0",
    "2400 tone 1 18", "2400 tone 1 -1", "2400 tone 1 1.5",
    "2400 tone 1 nan", "2400 tone 1 99999999999999999999999999",
]
valid = [
    (" 2390 tone 1", 2390, 1, 0),
    ("2450 two 60 17", 2450, 60, 17),
    ("\t2400\ttag\t2\t3\t", 2400, 2, 3),
]
# Commands handled by dvbtx.c: invalid arguments, then valid ones with the values each stub
# must receive (freq, modcod, seconds, gain, level, sps) for dvb, (modcod, frames, level, decim,
# stall, sps, DAC rate) for dvbdump and (modcod, seconds, level, stall, DAC rate, sps) for dvbpace.
dvbtx = {
    "dvb": (["", "2400", "2400 4", "2389 4 1", "2451 4 1", "2400 3 1", "2400 12 1",
             "2400 4 0", "2400 4 61", "2400 4 1 18", "2400 4 1 0 15", "2400 4 1 0 509",
             "2400 4 1 0 128 x", "2400 4 1.5", "2400 0x4 1"],
            [("2400 4 1", (2400, 4, 1, 0, 128, 40)), ("2450 11 60 17 508", (2450, 11, 60, 17, 508, 40)),
             ("2390 4 1 0 16", (2390, 4, 1, 0, 16, 40))]),
    "dvbdump": (["", "4", "3 1", "12 1", "4 0", "4 257", "4 1 15", "4 1 509", "4 1 128 0",
                 "4 1 128 3", "4 1 128 21", "4 1 128 10 2001", "4 1 128 10 0 x", "4 -1"],
                [("4 40", (4, 40, 128, 10, 0, 40)), ("11 256 508 1 2000", (11, 256, 508, 1, 2000, 40)),
                 ("6 1 64 20", (6, 1, 64, 20, 0, 40))]),
    "dvbpace": (["", "4", "4 0", "4 601", "3 1", "12 1", "4 1 15", "4 1 128 2001",
                 "4 1 128 0 x", "4 1.5"],
                [("4 30", (4, 30, 128, 0, 40000000, 40)),
                 ("11 600 64 25", (11, 600, 64, 25, 40000000, 40))]),
}

for name, (_, good) in dvbtx.items():
    for mc in range(12, 18):
        for rate, sps in (("5m", 8), ("10m", 4)):
            if name == "dvb":
                good.append((f"2400 {mc} {rate} 1", (2400, mc, 1, 0, 128, sps)))
                dvbtx[name][0].extend([f"2400 {mc} 1", f"2400 {mc} 1m 1"])
            elif name == "dvbdump":
                good.append((f"{mc} {rate} 40", (mc, 40, 128, sps // 4, 0, sps, 40000000)))
                good.append((f"{mc} {rate} 2 64 1 20", (mc, 2, 64, 1, 20, sps, 40000000)))
                dvbtx[name][0].extend([f"{mc} {rate} 40 128 10", f"{mc} {rate} 40 128 3"])
            else:
                good.append((f"{mc} {rate} 60 128 25", (mc, 60, 128, 25, 40000000, sps)))
                dvbtx[name][0].append(f"{mc} {rate} 601")
    # 20m: the 4-sps samples on the 80 MS/s schedule, radio-off commands only.
    if name == "dvb":
        dvbtx[name][0].extend([f"2400 {mc} 20m 1" for mc in (12, 13, 17)])
    elif name == "dvbdump":
        good += [(f"{mc} 20m 40", (mc, 40, 128, 1, 0, 4, 80000000)) for mc in (12, 13, 17)]
        dvbtx[name][0].extend(["13 20m 40 128 3", "4 20m 40", "13 20 40", "13 20M 40"])
    else:
        good += [(f"{mc} 20m 60 128 25", (mc, 60, 128, 25, 80000000, 4)) for mc in (12, 13, 17)]
        dvbtx[name][0].extend(["13 20m 601", "4 20m 1", "13 40m 1"])
    dvbtx[name][0].extend(["4 5m 1", "18 5m 1", "13 5m", "13 10m 1 128 0 extra"])

# wifimem: optional MODCOD (and rate for 8PSK); it reaches dvbtx_wifi(modcod, sps) only.
dvbtx["wifimem"] = (["x", "3", "18", "12", "12 1m", "4 5m", "13 5m extra", "4 extra", "0x4",
                     "13 10m 1", "13 5m 10m"],
                    [("", (0, 0)), ("4", (4, 40)), ("11", (11, 40)), ("13 5m", (13, 8)),
                     ("15 10m", (15, 4)), (" 17\t10m", (17, 4))])
dvbtx["wifimem"][0].extend(["13 20m", "17 20m"])

prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_s3.h"
static int signal_storage;
#define tx_signal ((void *)&signal_storage)
static bool configured = true;
static const char *station_store_get(void) { return configured ? "N0CALL" : ""; }

static unsigned calls, got_freq, got_seconds, got_gain;
static bool radio_signal_init(void *ctx, const char *name)
{
    (void)ctx;
    return !strcmp(name, "tone") || !strcmp(name, "two") || !strcmp(name, "tag");
}
static esp_err_t fill_signal(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)dst; (void)first; (void)count; (void)ctx;
    return ESP_OK;
}
esp_err_t radio_s3_run(uint32_t freq, uint8_t gain, uint32_t seconds,
                       radio_s3_fill_fn fill, void *ctx, radio_s3_stats_t *stats)
{
    assert(fill == fill_signal && ctx == tx_signal);
    calls++; got_freq = freq; got_seconds = seconds; got_gain = gain;
    memset(stats, 0, sizeof *stats);
    return ESP_OK;
}
static const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "ESP_OK";
}
static unsigned dvb_calls, dump_calls, pace_calls, wifi_calls, got[7];
static void stub(unsigned *count, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e,
                 unsigned f, unsigned g)
{
    ++*count;
    got[0] = a, got[1] = b, got[2] = c, got[3] = d, got[4] = e, got[5] = f, got[6] = g;
    printf("@{\"status\":\"ESP_OK\"}\n");
}
static void dvbtx_run(uint32_t freq, unsigned modcod, uint32_t seconds, unsigned gain,
                      unsigned level, unsigned sps)
{
    stub(&dvb_calls, freq, modcod, seconds, gain, level, sps, 0);
}
static void dvbtx_dump(unsigned modcod, unsigned frames, unsigned level, unsigned decim,
                       unsigned stall, unsigned sps, uint32_t rate)
{
    stub(&dump_calls, modcod, frames, level, decim, stall, sps, rate);
}
static void dvbtx_pace(unsigned modcod, uint32_t seconds, unsigned level, unsigned stall,
                       unsigned sps, uint32_t rate)
{
    stub(&pace_calls, modcod, seconds, level, stall, rate, sps, 0);
}
static void dvbtx_wifi(unsigned modcod, unsigned sps)
{
    stub(&wifi_calls, modcod, sps, 0, 0, 0, 0, 0);
}
#define RF_CALLS (calls + dvb_calls)
'''

with tempfile.TemporaryDirectory() as directory:
    tmp = pathlib.Path(directory)
    (tmp / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\n")
    checks = ["int main(void) {"]
    for command in invalid:
        checks.append("{ char args[] = " + json.dumps(command) +
                      "; cmd_tx(args); assert(calls == 0); }")
    for index, (command, freq, seconds, gain) in enumerate(valid, 1):
        checks.append("{ char args[] = " + json.dumps(command) + "; cmd_tx(args); " +
                      f"assert(calls == {index} && got_freq == {freq} && " +
                      f"got_seconds == {seconds} && got_gain == {gain}); }}")
    counters = {"dvb": "dvb_calls", "dvbdump": "dump_calls", "dvbpace": "pace_calls",
                "wifimem": "wifi_calls"}
    for name, (bad, good) in dvbtx.items():
        others = " && ".join(f"{v} == 0" for k, v in counters.items() if k != name)
        for command in bad:
            checks.append("{ char args[] = " + json.dumps(command) + f"; cmd_{name}(args); " +
                          "assert(dvb_calls + dump_calls + pace_calls + wifi_calls == 0); }")
        for index, (command, values) in enumerate(good, 1):
            expect = " && ".join(f"got[{i}] == {v}" for i, v in enumerate(values))
            checks.append("{ char args[] = " + json.dumps(command) + f"; cmd_{name}(args); " +
                          f"assert({counters[name]} == {index} && {others} && {expect}); }}")
        checks.append(f"{counters[name]} = 0;")
    checks.append("assert(RF_CALLS == " + str(len(valid)) + "); return 0; }")
    harness = tmp / "parser.c"
    # Missing ID rejects both keyed parsers before waveform init or encoder/radio
    # entry. The radio-off parser requests still reach their counted stubs.
    # 8PSK syntax too: the rate token must not move the refusal behind parsing.
    refused = ['{ char a[] = "2400 tone 1"; cmd_tx(a); assert(RF_CALLS == 0); }']
    refused += ['{ char a[] = "%s"; cmd_dvb(a); assert(RF_CALLS == 0); }' % a
                for a in ("2400 4 1", "2400 13 5m 1", "2400 15 10m 1")]
    allowed = ['{ char a[] = "%s"; %s(a); assert(%s == 1 && RF_CALLS == 0); %s = 0; }' % (a, f, c, c)
               for a, f, c in (("4 1", "cmd_dvbdump", "dump_calls"), ("4 1", "cmd_dvbpace", "pace_calls"),
                               ("13 5m 1", "cmd_dvbdump", "dump_calls"),
                               ("15 10m 1", "cmd_dvbpace", "pace_calls"))]
    checks[1:1] = ['configured = false;'] + refused + allowed + ['configured = true;']
    require_source = (ROOT / "firmware/main/station_commands.c").read_text()
    require_source = require_source[require_source.index("bool station_require("):require_source.index("void station_command(")]
    harness.write_text(prelude + require_source + parser + "\n".join(checks))
    executable = tmp / "parser"
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                    "-Werror", "-I", str(tmp), "-I",
                    str(ROOT / "firmware/components/radio_s3/include"), str(harness),
                    "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    records = [json.loads(line[1:]) for line in result.stdout.splitlines()]
    assert all("no callsign set" in r.get("error", "") for r in records[:len(refused)])
    assert all(r.get("status") == "ESP_OK" for r in records[len(refused):len(refused) + len(allowed)])
    records = records[len(refused) + len(allowed):]
    counts = [len(invalid), len(valid)]
    for bad, good in dvbtx.values():
        counts += [len(bad), len(good)]
    assert len(records) == sum(counts)
    start = 0
    for index, count in enumerate(counts):
        group = records[start:start + count]
        assert all(("error" in r) if index % 2 == 0 else r["status"] == "ESP_OK" for r in group)
        start += count

bad = len(invalid) + sum(len(b) for b, _ in dvbtx.values())
print(f"commands: {bad} invalid tx/dvb/dvbdump/dvbpace/wifimem requests stop in the parser; valid "
      "ones reach only their own function with the parsed values; dvbdump, dvbpace and wifimem "
      "never reach radio_s3_run or dvbtx_run")

# The lower-level public RF entry must fail closed too, before on_core1 can
# create the radio worker. Compile that entry verbatim against a counted worker.
radio = (ROOT / "firmware/components/radio_s3/radio_s3.c").read_text()
entry = radio[radio.index("esp_err_t radio_s3_run("):]
with tempfile.TemporaryDirectory() as directory:
    tmp = pathlib.Path(directory)
    (tmp / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n"
                                   "#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_INVALID_STATE 2\n")
    harness = tmp / "entry.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "radio_s3.h"
typedef struct { unsigned cycles; } geometry_t;
static geometry_t geometry(uint32_t rate) { return (geometry_t){240000000U / rate}; }
typedef struct {
    uint64_t samples;
    bool identify, limit_duration;
    unsigned freq_mhz, gain, seconds;
    radio_s3_fill_fn fill;
    void *ctx;
    radio_s3_stats_t *stats;
    radio_s3_chain_t chain;
    geometry_t geo;
} run_t;
static radio_s3_chain_t chain;
static bool configured;
static unsigned workers;
static const char *station_store_get(void) { return configured ? "N0CALL" : ""; }
static void worker(void *arg) { (void)arg; }
static int on_core1(void (*fn)(void *), run_t *run)
{
    assert(fn == worker && run->identify && run->limit_duration && run->samples == 40000000);
    assert(run->geo.cycles == 6);   // keyed runs stay at 40 MS/s until the ID is rate-aware
    workers++;
    return ESP_OK;
}
static int fill(uint32_t *dst, uint64_t first, size_t n, void *ctx)
{ (void)dst; (void)first; (void)n; (void)ctx; return 0; }
''' + entry + r'''
int main(void)
{
    radio_s3_stats_t s = {0};
    assert(radio_s3_run(2420, 0, 1, fill, NULL, &s) == ESP_ERR_INVALID_STATE);
    assert(workers == 0);
    configured = true;
    assert(radio_s3_run(2420, 0, 1, fill, NULL, &s) == ESP_OK && workers == 1);
}
''')
    exe = tmp / "entry"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
                    "-I", str(ROOT / "firmware/components/radio_s3/include"), str(harness),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("RF entry: missing callsign prevents worker creation; configured entry requires the ID wrapper")
