"""Cycle/cache interval arithmetic, including 32-bit wrap and 64-bit accumulation."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    src = pathlib.Path(tmp) / "profile.c"
    src.write_text(r'''
#include <assert.h>
#include "stream_profile.h"
int main(void) {
    stream_profile_t p = {0};
    stream_mark_t a = {UINT32_MAX - 9, UINT32_MAX, 50, 100, 70};
    stream_mark_t b = {10, 2, 55, 130, 70};
    stream_record(&p, a, b);
    assert(p.calls == 1 && p.cycles == 20 && p.max_cycles == 20);
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
