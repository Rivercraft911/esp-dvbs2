"""Host C tests of BBFRAME ordering, storage ownership and ordered TS deframing.
Run: python3 tests/test_rx_order.py. No ESP-IDF, board or pytest needed.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/components/dvbs2rx"

PROGRAM = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvbs2rx.h"

#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); exit(1); } } while (0)

static uint8_t mem[65536] __attribute__((aligned(16)));
static struct {
    uint32_t sequence;
    int length;
    uint8_t bytes[RX_HARD_BYTES];
} out[64];
static unsigned count;
static uint8_t *direct;

static void emit(void *ctx, uint32_t sequence, uint8_t *bbframe, int length)
{
    CHECK(ctx == out && count < 64);
    if (direct) CHECK(bbframe == direct);
    out[count].sequence = sequence;
    out[count].length = length;
    CHECK((bbframe != NULL) == (length >= 0));
    if (bbframe) {
        memcpy(out[count].bytes, bbframe, 10U + (unsigned)length);
        memset(bbframe, 0xcc, 10U + (unsigned)length);  /* like the deframer, output may mutate */
    }
    ++count;
}

static rx_order_t *init(unsigned capacity)
{
    CHECK(rx_order_size(capacity) + 16 < sizeof mem);
    memset(mem, 0xa5, sizeof mem);
    rx_order_t *o = (rx_order_t *)mem;
    rx_order_init(o, capacity, emit, out);
    count = 0;
    direct = NULL;
    return o;
}

static rx_order_stats_t stats(rx_order_t *o, unsigned capacity)
{
    rx_order_stats_t s;
    rx_order_stats(o, &s);
    /* Caller storage ends exactly at size(); the module must not write past it. */
    for (unsigned i = 0; i < 16; ++i) CHECK(mem[rx_order_size(capacity) + i] == 0xa5);
    return s;
}

static void exact(void)
{
    rx_order_t *o = init(4);
    uint8_t b[RX_HARD_BYTES];
    for (unsigned k = 0; k < 2; ++k) {
        memset(b, 0x10 + k, sizeof b);
        direct = b;
        rx_order_frame(o, k, b, k ? RX_HARD_BYTES - 10 : 0);
        CHECK(count == k + 1 && out[k].sequence == k);
        for (unsigned j = 0; j < (k ? sizeof b : 10); ++j) CHECK(out[k].bytes[j] == 0x10 + k);
    }
    direct = NULL;
    rx_order_frame(o, 2, NULL, -1);
    CHECK(count == 3 && out[2].length == -1);
    rx_order_drain(o);
    rx_order_stats_t s = stats(o, 4);
    CHECK(!s.held && !s.max_held && !s.repaired && !s.given_up);
}

static void results(void)
{
    rx_order_t *o = init(8);
    uint8_t b[RX_HARD_BYTES];
    rx_order_wait(o, 20);
    memset(b, 0x21, sizeof b);
    rx_order_frame(o, 21, b, 5);
    memset(b, 0, sizeof b);                       /* source slot is immediately reusable */
    rx_order_wait(o, 22);
    rx_order_wait(o, 24);                         /* a missing sequence is not a wait */
    rx_order_frame(o, 25, NULL, -1);
    memset(b, 0x26, sizeof b);
    rx_order_frame(o, 26, b, RX_HARD_BYTES - 10);
    memset(b, 0, sizeof b);
    CHECK(count == 0);
    CHECK(rx_order_result(o, 24, NULL, -1));       /* out-of-order failure cannot pass 20 */
    CHECK(rx_order_result(o, 22, b, 0));           /* empty repaired data field, 10 header bytes */
    rx_order_drain(o);
    CHECK(count == 0);
    memset(b, 0x20, sizeof b);
    CHECK(rx_order_result(o, 20, b, 7));
    memset(b, 0xff, sizeof b);                    /* result storage is reusable before drain */
    CHECK(!rx_order_result(o, 20, b, 7));          /* duplicate result */
    rx_order_drain(o);
    CHECK(count == 6);
    const uint32_t sequences[] = {20, 21, 22, 24, 25, 26};
    const int lengths[] = {7, 5, 0, -1, -1, RX_HARD_BYTES - 10};
    for (unsigned i = 0; i < count; ++i) {
        CHECK(out[i].sequence == sequences[i] && out[i].length == lengths[i]);
        for (int j = 0; lengths[i] >= 0 && j < 10 + lengths[i]; ++j)
            CHECK(out[i].bytes[j] == (i == 0 ? 0x20 : i == 1 ? 0x21 : i == 2 ? 0 : 0x26));
    }
    CHECK(!rx_order_result(o, 24, NULL, -1));
    rx_order_stats_t s = stats(o, 8);
    CHECK(s.held == 6 && s.max_held == 6 && s.repaired == 2 && !s.given_up);
}

static void full(void)
{
    rx_order_t *o = init(3);
    uint8_t b[11] = {0};
    rx_order_wait(o, 0);
    rx_order_frame(o, 1, b, 1);
    rx_order_wait(o, 2);
    rx_order_frame(o, 3, b, 1);                    /* give up 0, drain 1, stop at 2 */
    CHECK(count == 2 && out[0].sequence == 0 && out[0].length == -1 && out[1].sequence == 1);
    CHECK(!rx_order_result(o, 0, b, 1));           /* late repaired frame cannot re-enter TS */
    rx_order_wait(o, 4);                          /* wrap the ring's tail */
    rx_order_wait(o, 5);                          /* give up 2 and drain 3, stop at 4 */
    CHECK(count == 4 && out[2].sequence == 2 && out[2].length == -1 && out[3].sequence == 3);
    CHECK(!rx_order_result(o, 2, NULL, -1));        /* late failure is ignored too */
    CHECK(rx_order_result(o, 5, b, 1));
    rx_order_drain(o);
    CHECK(count == 4);
    CHECK(rx_order_result(o, 4, NULL, -1));
    rx_order_drain(o);
    CHECK(count == 6 && out[4].length == -1 && out[5].length == 1);
    rx_order_stats_t s = stats(o, 3);
    CHECK(s.held == 6 && s.max_held == 3 && s.repaired == 1 && s.given_up == 2);

    o = init(1);
    rx_order_wait(o, UINT32_MAX);
    rx_order_wait(o, 0);                          /* full on a wait, including sequence wrap */
    CHECK(count == 1 && out[0].sequence == UINT32_MAX && out[0].length == -1);
    CHECK(!rx_order_result(o, UINT32_MAX, b, 1));
    CHECK(rx_order_result(o, 0, b, 1));
    rx_order_drain(o);
    rx_order_frame(o, 3, b, 1);                    /* an idle gap stays a gap in sequence */
    CHECK(count == 3 && out[1].sequence == 0 && out[2].sequence == 3);
    s = stats(o, 1);
    CHECK(s.held == 2 && s.max_held == 1 && s.repaired == 1 && s.given_up == 1);
}

static uint8_t ts_mem[8192] __attribute__((aligned(16)));
static uint8_t packets[6][188], frames[6][198];
static unsigned packet_count, mask;

static unsigned crc8(const uint8_t *p, unsigned n)
{
    unsigned c = 0;
    while (n--) {
        c ^= *p++;
        for (unsigned k = 0; k < 8; ++k) c = ((c << 1) ^ (c & 128 ? 0xd5 : 0)) & 255;
    }
    return c;
}

static void packet(void *ctx, const uint8_t *p)
{
    CHECK(ctx == packets);
    unsigned i = 0;
    while (i < 6 && !(mask & (1U << i))) ++i;
    CHECK(i < 6 && !memcmp(p, packets[i], 188));
    mask &= ~(1U << i);
    ++packet_count;
}

static void deframe(void *ctx, uint32_t sequence, uint8_t *bbframe, int length)
{
    /* Same gap policy as receive.c: the next accepted sequence triggers resync. */
    if (bbframe) rx_ts_frame(ctx, bbframe, (unsigned)length, sequence);
}

static void ts(unsigned scenario)
{
    rx_order_t *o = init(scenario == 2 ? 2 : 8);
    rx_ts_t *t = (rx_ts_t *)ts_mem;
    CHECK(rx_ts_size() <= sizeof ts_mem);
    rx_ts_init(t, 0, packet, packets);
    rx_order_init(o, scenario == 2 ? 2 : 8, deframe, t);
    packet_count = 0;
    mask = scenario == 0 ? 0x3f : scenario == 1 ? 0x3d : 0x3e;
    for (unsigned i = 0; i < 6; ++i) {
        memset(packets[i], 0x10 + i, 188);
        packets[i][0] = 0x47;
        packets[i][1] = 0x01;
        packets[i][2] = 0;
        packets[i][3] = 0x10 + i;
        const uint8_t h[10] = {0xf0, 0, 0x05, 0xe0, 0x05, 0xe0, 0x47, 0, 0, 0};
        memcpy(frames[i], h, 10);
        memcpy(frames[i] + 10, packets[i], 188);
        frames[i][10] = i ? (uint8_t)crc8(packets[i - 1] + 1, 187) : 0;
    }
    if (scenario == 2) {
        rx_order_wait(o, 0);
        rx_order_frame(o, 1, frames[1], 188);
        rx_order_frame(o, 2, frames[2], 188);       /* storage gives up frame 0 */
        CHECK(!rx_order_result(o, 0, frames[0], 188));
    } else {
        rx_order_frame(o, 0, frames[0], 188);
        rx_order_wait(o, 1);
        rx_order_wait(o, 2);
        rx_order_frame(o, 3, frames[3], 188);
        CHECK(rx_order_result(o, 2, frames[2], 188));
        CHECK(rx_order_result(o, 1, scenario ? NULL : frames[1], scenario ? -1 : 188));
        rx_order_drain(o);
    }
    for (unsigned i = scenario == 2 ? 3 : 4; i < 6; ++i)
        rx_order_frame(o, i, frames[i], 188);
    rx_order_drain(o);
    rx_ts_flush(t);                               /* the end, after all results were taken */
    CHECK(mask == 0 && packet_count == (scenario ? 5U : 6U));
    rx_ts_stats_t s;
    rx_ts_stats(t, &s);
    CHECK(!s.late && !s.crc_errors && !s.syncd_errors);
    CHECK(s.frames == packet_count && s.unchecked == (scenario == 1 ? 2U : 1U));
    CHECK(s.resyncs == (scenario == 1 ? 1U : 0U));
}

int main(void)
{
    exact();
    results();
    full();
    for (unsigned i = 0; i < 3; ++i) ts(i);
    puts("ok");
    return 0;
}
"""

MEMORY = r"""
#include <stdio.h>
#include "arena.h"
#include "receive_buffers.h"

#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

int main(void)
{
    static const unsigned modes[] = {4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16};
    CHECK(arena_check() == 0);
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; ++i) {
        unsigned m = modes[i];
        arena_reset();
        CHECK(arena_take(&arena_low, rx_demod_size()));
        CHECK(arena_take(&arena_low, rx_fec_check_size(m)));
        CHECK(arena_take(&arena_low, rx_ts_size()));
        CHECK(arena_take(&arena_high, RX_DEMOD_RING_BYTES));
        for (unsigned j = 0; j < 3; ++j) {
            CHECK(arena_take(&arena_high, RX_HARD_BYTES));
            CHECK(arena_take(&arena_high, RX_HARD_BYTES));
        }
        CHECK(arena_take(&arena_psram, rx_fec_size(m)));
        CHECK(arena_take(&arena_psram, rx_fec_work_size(m)));
        decode_job_t *jobs = arena_take(&arena_psram, JOBS * sizeof *jobs);
        CHECK(jobs);
        for (unsigned j = 0; j < JOBS; ++j) {
#define PLANE(name) CHECK(!((uintptr_t)jobs[j].name & 15));
            JOB_PLANES(PLANE)
#undef PLANE
        }
        CHECK(arena_take(&arena_psram, rx_order_size(ORDER_FRAMES)));
        CHECK(arena_take(&arena_psram, 1024));
        printf("%u: low %zu, high %zu, psram %zu\n", m, arena_low.used, arena_high.used, arena_psram.used);
    }
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = pathlib.Path(tmp) / "rx_order.c", pathlib.Path(tmp) / "rx_order"
        src.write_text(PROGRAM)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(COMPONENT / "include"),
            str(src), str(COMPONENT / "rx_order.c"), str(COMPONENT / "rx_ts.c"),
            "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0 and out.stdout.strip() == "ok", out.stdout + out.stderr
        # The real arena_check and allocation sizes, including the S3 deframer's CRC table.
        # Only placement attributes are stubbed; these are host ABI sizes, not board timings.
        folder = pathlib.Path(tmp)
        (folder / "esp_attr.h").write_text("#define EXT_RAM_BSS_ATTR\n")
        (folder / "esp_heap_caps.h").write_text("")
        (folder / "heap_memory_layout.h").write_text("#define SOC_RESERVE_MEMORY_REGION(a,b,c)\n")
        src.write_text(MEMORY)
        encoder = ROOT / "firmware/components/dvbs2"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DRX_TS_COLUMN_MODEL",
            "-I", str(folder), "-I", str(COMPONENT / "include"), "-I", str(COMPONENT),
            "-I", str(encoder / "include"), "-I", str(ROOT / "firmware/rx/main"),
            "-I", str(ROOT / "firmware/components/platform/include"), str(src),
            str(ROOT / "firmware/rx/main/arena.c"), str(COMPONENT / "rx_order.c"),
            str(COMPONENT / "rx_ts.c"), str(COMPONENT / "rx_fec.c"), str(COMPONENT / "demod.c"),
            str(COMPONENT / "kernels.c"), str(encoder / "dvbs2.c"), str(encoder / "fast.c"),
            str(ROOT / "tests/ts_crc_model.c"), "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("rx order: direct and copied frames, repairs, failures, full storage, late results, "
          "sequence gaps/wrap, ordered TS packets and every mode's arena allocations: ok")


if __name__ == "__main__":
    main()
