"""The run-memory allocator (firmware/components/platform/include/run_arena.h arena_take) on the host: 16-byte
alignment, filling exactly, and refusing sizes that would wrap or an arena already past its end.
Run: python3 tests/test_run_arena.py. No ESP-IDF, board or pytest needed.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

PROGRAM = r"""
#include <stdio.h>
#include "run_arena.h"

#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

int main(void)
{
    static uint8_t mem[256] __attribute__((aligned(16)));
    arena_t a = {mem, sizeof mem, 0, 0};
    uint8_t *p = arena_take(&a, 1), *q = arena_take(&a, 17), *r = arena_take(&a, 0);
    CHECK(p == mem && q == mem + 16 && r == mem + 48 && a.used == 48 && a.peak == 48);
    CHECK(arena_take(&a, 209) == NULL && a.used == 48);          /* 224 rounded, 208 left */
    CHECK(arena_take(&a, 208) == mem + 48 && a.used == 256);     /* exactly full */
    CHECK(arena_take(&a, 1) == NULL && a.used == 256);
    a.used = 0;
    for (size_t n = SIZE_MAX - 20; n != 0; ++n)                  /* would round past SIZE_MAX */
        CHECK(arena_take(&a, n) == NULL && a.used == 0);
    a.used = a.size + 16;                                        /* corrupt: past its end */
    CHECK(arena_take(&a, 0) == NULL && arena_take(&a, 16) == NULL && a.used == a.size + 16);
    CHECK(a.peak == 256);
    printf("ok\n");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = pathlib.Path(tmp) / "arena_test.c", pathlib.Path(tmp) / "arena_test"
        src.write_text(PROGRAM)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "firmware/components/platform/include"),
            str(src), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0 and out.stdout.strip() == "ok", out.stdout
    print("run arena: alignment, exact fill, wrapping sizes and an overrun arena refused: ok")


if __name__ == "__main__":
    main()
