"""TS adaptation's CRC-8 by columns (fast.c, ts_pie.S) on the host, with a C model of the PIE
kernel that makes the same memory reads: 14 aligned 16-byte blocks from align_down(p, 16).
Each input ends exactly at a page with no access, so a read past the caller's buffer faults.
Frames are compared byte for byte with the plain encoder (dvbs2.c) over consecutive frames of
every QPSK and 8PSK rate, carrying the TS state. Run: python3 tests/test_ts_crc.py.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

PROGRAM = r"""
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dvbs2.h"

#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

static unsigned model_calls;
/* The kernel's reads (ts_pie.S), then the CRC by columns, positions 0..191 of the packet. */
uint32_t dvbs2_pie_crc8_packet(const uint8_t *p, const uint8_t *cols)
{
    const volatile uint8_t *first = (const volatile uint8_t *)((uintptr_t)p & ~(uintptr_t)15);
    uint8_t touched = 0, crc = 0;
    for (unsigned i = 0; i < 224; ++i) touched ^= first[i];
    for (unsigned i = 0; i < 192; ++i)
        for (unsigned k = 0; k < 8; ++k)
            if ((p[i] >> k) & 1) crc ^= cols[(i / 16) * 128 + (7 - k) * 16 + i % 16];
    (void)touched;
    ++model_calls;
    return crc;
}

int main(void)
{
    const long page = sysconf(_SC_PAGESIZE);
    const size_t span = 4 * (size_t)page;
    uint8_t *map = mmap(NULL, span + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(map != MAP_FAILED && !mprotect(map + span, page, PROT_NONE));
    static dvbs2_workspace plain, fastws;
    unsigned modes = 0;
    for (unsigned mc = 4; mc <= 17; ++mc) {
        const dvbs2_mode *mode = dvbs2_find_mode(mc);
        if (!mode) continue;
        const size_t n = ((size_t)mode->kbch - 80) / 8;
        dvbs2_fast *f = aligned_alloc(16, (dvbs2_fast_mode_size(mode) + 15) & ~(size_t)15);
        CHECK(f && !dvbs2_fast_prepare(f, mode, 1, 15));
        uint8_t *input = map + span - n;            /* ends at the page with no access */
        dvbs2_ts_state a = {0}, b = {0};
        uint32_t x = 2026 + mc;
        unsigned seq = 0;
        for (unsigned frame = 0; frame < 6; ++frame) {
            /* A continuing TS: sync, then pseudo-random bytes, packets across frames. */
            for (size_t i = 0; i < n; ++i) {
                const unsigned pos = (seq + i) % 188;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                input[i] = pos ? (uint8_t)x : 0x47;
            }
            seq = (seq + n) % 188;
            CHECK(!dvbs2_bb_ts(&plain, mode, &a, input, n));
            dvbs2_bb_scramble(&plain, mode);
            CHECK(!dvbs2_fast_bb_ts(f, &fastws, &b, input, n));
            CHECK(!memcmp(plain.bits, fastws.bits, mode->kbch / 8));
            CHECK(a.packet_pos == b.packet_pos && a.crc == b.crc && a.previous_crc == b.previous_crc);
        }
        free(f);
        ++modes;
    }
    CHECK(modes == 14 && model_calls > 100);
    printf("ok %u\n", model_calls);
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        src, exe = folder / "ts_crc.c", folder / "ts_crc"
        src.write_text(PROGRAM)
        dvbs2 = ROOT / "firmware/components/dvbs2"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DDVBS2_TS_COLUMN_MODEL",
            "-I", str(dvbs2 / "include"), "-I", str(dvbs2), str(src), str(dvbs2 / "dvbs2.c"),
            str(dvbs2 / "fast.c"), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0 and out.stdout.startswith("ok"), (out.returncode, out.stdout, out.stderr)
    print(f"TS CRC-8 by columns: 14 modes x 6 frames match the plain encoder; the kernel's reads "
          f"stay inside inputs that end at a guard page ({out.stdout.split()[1]} packets): ok")


if __name__ == "__main__":
    main()
