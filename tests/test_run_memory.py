"""The transmitter's run memory (firmware/tx/main/run_memory.c) on the host: first fit across the
two internal regions, PSRAM by caps, 32-byte alignment, zeroed takes, reset and peaks, and two
threads taking at once without overlap. Run: python3 tests/test_run_memory.py. No board.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

PROGRAM = r"""
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "run_memory.h"

#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
#define INSIDE(p, a) ((uint8_t *)(p) >= (a).base && (uint8_t *)(p) < (a).base + (a).size)

static uint8_t *taken[2][2000];
static void *taker(void *arg)
{
    uint8_t **out = arg;
    for (unsigned i = 0; i < 2000; ++i) out[i] = run_take(48, 16, MALLOC_CAP_SPIRAM);
    return NULL;
}

int main(void)
{
    CHECK(!run_memory_init());
    CHECK(run_internal[0].size == RUN_LOW_BYTES && run_internal[1].size == RUN_HIGH_BYTES);
    CHECK(RUN_LOW_BYTES == 38672 && RUN_HIGH_BYTES == 32768);
    uint8_t *a = run_take(30000, 16, MALLOC_CAP_INTERNAL);
    memset(a, 0xa5, 30000);
    uint8_t *b = run_take(20000, 16, MALLOC_CAP_INTERNAL);    /* no room left low: first fit high */
    uint8_t *c = run_take(8000, 16, MALLOC_CAP_INTERNAL);     /* fits low again */
    CHECK(INSIDE(a, run_internal[0]) && INSIDE(b, run_internal[1]) && INSIDE(c, run_internal[0]));
    CHECK(c == a + 30000 && run_internal[0].used == 38000 && run_internal[1].used == 20000);
    CHECK(!run_take(20000, 16, MALLOC_CAP_INTERNAL));         /* fits neither; nothing taken */
    CHECK(run_internal[0].used == 38000 && run_internal[1].used == 20000);
    uint8_t *d = run_take(100, 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *e = run_take(1, 32, MALLOC_CAP_SPIRAM);
    CHECK(INSIDE(d, run_psram) && !((uintptr_t)d & 31) && e == d + 128 && run_psram.used == 144);
    run_reset();
    CHECK(!run_internal[0].used && !run_internal[1].used && !run_psram.used);
    CHECK(run_internal[0].peak == 38000 && run_internal[1].peak == 20000 && run_psram.peak == 144);
    uint8_t *z = run_take(30000, 16, MALLOC_CAP_INTERNAL);     /* the same bytes, zeroed again */
    CHECK(z == a);
    for (unsigned i = 0; i < 30000; ++i) CHECK(!z[i]);
    run_reset();
    pthread_t t[2];
    for (unsigned i = 0; i < 2; ++i) CHECK(!pthread_create(&t[i], NULL, taker, taken[i]));
    for (unsigned i = 0; i < 2; ++i) CHECK(!pthread_join(t[i], NULL));
    CHECK(run_psram.used == 2 * 2000 * 48);                    /* every take distinct */
    static uint8_t owner[RUN_PSRAM_BYTES / 16];
    for (unsigned k = 0; k < 2; ++k)
        for (unsigned i = 0; i < 2000; ++i) {
            const size_t at = (size_t)(taken[k][i] - run_psram.base) / 16;
            for (unsigned j = 0; j < 3; ++j) CHECK(!owner[at + j]++);
        }
    printf("ok\n");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        (folder / "esp_heap_caps.h").write_text(
            "#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_INTERNAL 1\n#define MALLOC_CAP_8BIT 2\n"
            "#define MALLOC_CAP_SPIRAM 4\nstatic inline void *heap_caps_aligned_alloc(size_t a, size_t n,"
            " unsigned c) { (void)a; (void)n; (void)c; return NULL; }\n")
        src, exe = folder / "run_memory_test.c", folder / "run_memory_test"
        src.write_text(PROGRAM)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-D_POSIX_C_SOURCE=200809L",
            "-I", str(folder), "-I", str(ROOT / "firmware/tx/main"),
            "-I", str(ROOT / "firmware/components/platform/include"),
            str(src), str(ROOT / "firmware/tx/main/run_memory.c"), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0 and out.stdout.strip() == "ok", out.stdout
    print("tx run memory: first fit across internal regions, PSRAM by caps, alignment, zeroing, "
          "reset, peaks and concurrent takes: ok")


if __name__ == "__main__":
    main()
