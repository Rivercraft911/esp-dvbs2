"""Compile the actual console parser against a counted RF stub. No board is used."""
import json
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Extract just the two parser functions so this safety check needs no simulated
# RTOS, USB or Wi-Fi stack. Every tested branch is compiled from main.c itself.
source = (ROOT / "firmware/main/main.c").read_text()
parser = (source[source.index("static bool number("):source.index("static void cmd_dvb(")] +
          source[source.index("static void cmd_tx("):source.index("static void cmd_info(")])
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

prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_s3.h"
#define signal test_signal
static int signal;
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
    assert(fill == fill_signal && ctx == &signal);
    calls++; got_freq = freq; got_seconds = seconds; got_gain = gain;
    memset(stats, 0, sizeof *stats);
    return ESP_OK;
}
static const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "ESP_OK";
}
'''

with tempfile.TemporaryDirectory() as directory:
    tmp = pathlib.Path(directory)
    (tmp / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n")
    checks = ["int main(void) {"]
    for command in invalid:
        checks.append("{ char args[] = " + json.dumps(command) +
                      "; cmd_tx(args); assert(calls == 0); }")
    for index, (command, freq, seconds, gain) in enumerate(valid, 1):
        checks.append("{ char args[] = " + json.dumps(command) + "; cmd_tx(args); " +
                      f"assert(calls == {index} && got_freq == {freq} && " +
                      f"got_seconds == {seconds} && got_gain == {gain}); }}")
    checks.append("return 0; }")
    harness = tmp / "parser.c"
    harness.write_text(prelude + parser + "\n".join(checks))
    executable = tmp / "parser"
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                    "-Werror", "-I", str(tmp), "-I",
                    str(ROOT / "firmware/components/radio_s3/include"), str(harness),
                    "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    records = [json.loads(line[1:]) for line in result.stdout.splitlines()]
    assert len(records) == len(invalid) + len(valid)
    assert all("error" in record for record in records[:len(invalid)])
    assert all(record["status"] == "ESP_OK" for record in records[len(invalid):])

print(f"tx command: {len(invalid)} invalid requests never enter RF; boundary/default cases pass")
