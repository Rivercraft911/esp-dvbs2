"""Compile the independent C waveform generator and check sample words and bank seams.

No ESP-IDF or hardware is needed: python3 tests/test_radio.py. These tests verify the
software timeline model. Only RF capture can establish whether the DAC follows that model.
With a log argument, checks the QEMU run of radio_s3's dry run instead (tests/radio_qemu/README.md).
"""
import ctypes as ct
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import capture

COMPONENT = ROOT / "firmware/components/radio_s3"
MASK = 0x80200003
N, LEAD, TAIL = 16380, 64, 2048
STEP = N - LEAD - TAIL


class Signal(ct.Structure):
    _fields_ = [("tag", ct.c_bool), ("sweep", ct.c_bool),
                ("bandwidth_40m", ct.c_uint32), ("tone", ct.c_uint32 * 80),
                ("rows", (ct.c_uint32 * 8) * 256), ("advance", ct.c_uint32 * 256),
                ("jump", (ct.c_uint32 * 32) * 32)]


def step(s):
    return (s >> 1) ^ (MASK if s & 1 else 0)


def check_tag_period():
    # Binary matrix powers prove the seed's orbit order without iterating 2^32 states.
    def apply(matrix, state):
        out = 0
        for bit in range(32):
            if state >> bit & 1:
                out ^= matrix[bit]
        return out

    def advance(count):
        matrix = [step(1 << k) for k in range(32)]
        state = 1
        while count:
            if count & 1:
                state = apply(matrix, state)
            matrix = [apply(matrix, column) for column in matrix]
            count >>= 1
        return state

    order = (1 << 32) - 1
    factors = [3, 5, 17, 257, 65537]
    assert np.prod(factors, dtype=np.uint64) == order
    assert advance(order) == 1
    for prime in factors:
        assert advance(order // prime) != 1, prime


def tag_words(first, count):
    # Independent bit-by-bit definition, deliberately not the C eight-word row algorithm.
    matrix = [step(1 << k) for k in range(32)]

    def apply(m, x):
        return np.bitwise_xor.reduce([m[k] for k in range(32) if x >> k & 1],
                                    initial=0).item()

    state, n = 1, first % 0xFFFFFFFF
    while n:
        if n & 1:
            state = apply(matrix, state)
        matrix = [apply(matrix, x) for x in matrix]
        n >>= 1
    words = []
    for _ in range(count):
        words.append(256 if state & 1 else 768)
        state = step(state)
    return np.array(words, np.uint32)


def unpack(words):
    i, q = (words & 1023).astype(int), ((words >> 10) & 1023).astype(int)
    return (i - 1024 * (i >= 512)) + 1j * (q - 1024 * (q >= 512))


def run_checks(lib):
    check_tag_period()
    lib.radio_signal_init.argtypes = [ct.POINTER(Signal), ct.c_char_p]
    lib.radio_signal_init.restype = ct.c_bool
    lib.radio_signal_fill.argtypes = [ct.POINTER(ct.c_uint32), ct.c_uint64,
                                     ct.c_size_t, ct.POINTER(Signal)]
    signal = Signal()

    def fill(first, count):
        out = np.full(count + 2, 0xABCD1234, np.uint32)
        ptr = ct.cast(out.ctypes.data + 4, ct.POINTER(ct.c_uint32))
        lib.radio_signal_fill(ptr, first, count, ct.byref(signal))
        assert out[0] == out[-1] == 0xABCD1234
        return out[1:-1]

    assert not lib.radio_signal_init(ct.byref(signal), b"bad")
    for name, bins in [(b"tone", [1]), (b"two", [1, 3])]:
        assert lib.radio_signal_init(ct.byref(signal), name)
        words = fill(0, 400)
        z = unpack(words)
        assert (words >> 20 == 0).all()
        assert np.max(np.abs(z.real)) <= 256 and np.max(np.abs(z.imag)) <= 256
        # This sign check catches the TX/RX field reversal in the supplied context.
        spectrum = np.fft.fft(z[:40])
        peaks = np.argsort(np.abs(spectrum))[-len(bins):]
        assert sorted(peaks.tolist()) == bins
        for first, count in [(0, 0), (1, 1), (39, 81), (14204, N), (2399999999, 83)]:
            got = fill(first, count)
            assert np.array_equal(got, words[(np.arange(count) + first) % 40])
    assert lib.radio_signal_init(ct.byref(signal), b"gap")
    gap_words = fill(0, 40)
    assert np.max(np.abs(unpack(gap_words))) <= 128 * 2 ** .5
    assert np.argmax(np.abs(np.fft.fft(unpack(gap_words)))) == 1
    assert lib.radio_signal_init(ct.byref(signal), b"sweep")
    # Independent closed-form definition, including every dwell boundary, split fills,
    # bank wraps and the repeated 10 s period. Nearest integer with ties away from zero.
    for step_number in range(81):
        for delta in (-83, -1, 0, 1, 39):
            first = max(0, step_number * 20000000 + delta)
            count = 257
            t = np.arange(first, first + count, dtype=np.uint64)
            segment = (t // 20000000) % 40
            freq = np.where(segment < 20, segment.astype(int) + 1, 19 - segment.astype(int))
            phase = 2 * np.pi * ((t % 80).astype(int) * freq % 80) / 80
            def rnd(x):
                return np.copysign(np.floor(abs(x) + .5), x).astype(np.int32)
            i, q = rnd(64 * np.cos(phase)), rnd(64 * np.sin(phase))
            expected = ((i & 1023) | ((q & 1023) << 10)).astype(np.uint32)
            assert np.array_equal(fill(first, count), expected), (step_number, delta)
    assert signal.bandwidth_40m == 20000000
    assert lib.radio_signal_init(ct.byref(signal), b"tag")
    for first in [0, 1, 7, 31, STEP - LEAD, 2 * STEP - LEAD,
                  2399999999, 0xFFFFFFFF - 9, 0xFFFFFFFF + 3]:
        for count in [0, 1, 7, 8, 17, N]:
            got = fill(first, count)
            assert np.array_equal(got, tag_words(first, count)), (first, count)
            assert np.array_equal((got & 1023) == 256, capture.tag_bits(first, count))

    # Reproduce the driver's split-bank mapping for 40 handoffs. Both banks must agree
    # at every physical word in the complete 64-lead/2048-tail switch window, including wraps.
    previous = fill(0, N)
    for k in range(1, 41):
        first = k * STEP - LEAD
        bank = np.zeros(N, np.uint32)
        offset = first % N
        part = N - offset
        bank[offset:] = fill(first, part)
        bank[:offset] = fill(first + part, offset)
        shared = np.arange(k * STEP - LEAD, k * STEP + TAIL) % N
        assert np.array_equal(bank[shared], previous[shared]), k
        previous = bank
    print("radio: C tone sign/levels, two-tone offsets, PRBS seeks, guard words, 40 bank seams pass")


def check_gaps():
    """radio_s3.c's switch probe (silence) compiled from the source: at every switch the DAC
    plays |switch sample - handoff| silent samples, for any switch inside the overlap."""
    source = (COMPONENT / "radio_s3.c").read_text()
    start = source.index("static void IRAM_ATTR silence(")
    body = source[start:source.index("\n}\n", start) + 3]
    harness = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_s3.h"
#define IRAM_ATTR
typedef struct { uint32_t rate_bit, cycles, lead, tail, step; } geometry_t;
typedef struct { geometry_t geo; } run_t;
""" + body + r"""
static uint32_t banks[2][RADIO_S3_BANK_WORDS];
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    run_t run = {{0, 0, (uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]), 0}};
    run.geo.step = RADIO_S3_BANK_WORDS - run.geo.lead - run.geo.tail;
    const uint64_t step = run.geo.step, lead = run.geo.lead, tail = run.geo.tail;
    unsigned checked = 0;
    for (uint64_t k = 1; k < 4; ++k) {
        // Banks for handoffs k-1 and k, every word the tone (1), then the probe.
        const uint64_t old_start = k == 1 ? 0 : (k - 1) * step - lead, new_start = k * step - lead;
        for (unsigned i = 0; i < RADIO_S3_BANK_WORDS; ++i) banks[0][i] = banks[1][i] = 1;
        silence(&run, banks[0], old_start);
        silence(&run, banks[1], new_start);
        const int64_t h = (int64_t)(k * step);
        for (int64_t s = h - (int64_t)lead; s < h + (int64_t)tail; ++s) {
            unsigned zeros = 0;
            for (int64_t n = h - (int64_t)lead - 8; n < h + (int64_t)tail + 8; ++n)
                zeros += banks[n < s ? 0 : 1][n % RADIO_S3_BANK_WORDS] == 0;
            if (zeros != (unsigned)llabs(s - h)) { printf("FAIL k %llu s-h %lld zeros %u\n",
                (unsigned long long)k, (long long)(s - h), zeros); return 1; }
            ++checked;
        }
    }
    printf("ok %u\n", checked);
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c_file, exe = pathlib.Path(tmp) / "gaps.c", pathlib.Path(tmp) / "gaps"
        c_file.write_text(harness)
        (pathlib.Path(tmp) / "esp_err.h").write_text("typedef int esp_err_t;\n")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I",
                        str(COMPONENT / "include"), "-I", tmp, str(c_file), "-o", str(exe)],
                       check=True)
        for lead, tail in ((64, 2048), (128, 512)):
            out = subprocess.run([str(exe), str(lead), str(tail)], capture_output=True,
                                 text=True, check=True).stdout
            assert out == f"ok {3 * (lead + tail)}\n", out
    print("gap probe: the DAC plays |switch - handoff| silent samples for every switch in the "
          "overlap, both geometries")


def check_qemu(path):
    """tests/radio_qemu.c: radio_s3's dry run at both bank geometries (radio_s3.h)."""
    geometry = {40000000: (64, 2048), 80000000: (128, 512)}
    records = [json.loads(line[1:]) for line in pathlib.Path(path).read_text().splitlines()
               if line.startswith('@{"test":')]
    dry = [r for r in records if r["test"] == "dry"]
    assert len(dry) == 6 and {r["rate"] for r in dry} == set(geometry), records
    for r in dry:
        lead, tail = geometry[r["rate"]]
        step = N - lead - tail
        banks = -(-r["samples"] // step)
        assert r["status"] == 0 and r["bad"] == 0 and r["overlap_mismatches"] == 0, r
        assert r["played"] == r["samples"] and r["sink_calls"] == banks, r
        assert r["fills"] == banks + 1 and r["overlap_words"] == banks * (lead + tail), r
    assert [r["status"] for r in records if r["test"] == "bad_rate"] == [0x102]
    print(f"qemu: radio_s3 dry run at 40 and 80 MS/s geometry, {sum(r['played'] for r in dry)} "
          "played samples in order and correct, both banks equal over every lead and tail")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        check_qemu(sys.argv[1])
        sys.exit()
    with tempfile.TemporaryDirectory() as tmp:
        library = pathlib.Path(tmp) / "signals.so"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared",
                        "-fPIC", "-I", str(COMPONENT / "include"),
                        str(COMPONENT / "radio_signals.c"), "-lm", "-o", str(library)], check=True)
        run_checks(ct.CDLL(str(library)))
    check_gaps()
