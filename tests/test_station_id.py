"""Compile the real CW/multiplexer on the host. No device is opened."""
import ctypes as C
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import morse
import onair

HEADER = ROOT / "firmware/components/station_id/include"
SOURCE = ROOT / "firmware/components/station_id/station_id.c"
RATE = 40_000_000
PERIOD = RATE * 540
DOT = RATE * .06


def run():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        libpath = tmp / "id.so"
        radio_source = (ROOT / "firmware/components/radio_s3/radio_s3.c").read_text()
        preparation = radio_source[radio_source.index("    station_id_t *id = NULL;"):
                                   radio_source.index("    run->done = xSemaphoreCreateBinaryStatic")]
        probe = tmp / "budget.c"
        probe.write_text(r'''#include <stdlib.h>
#include "station_id.h"
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define heap_caps_calloc(n, s, flags) calloc(n, s)
#define heap_caps_free free
struct stats { uint32_t id_samples, id_count; uint64_t planned_samples; };
typedef struct {
    bool identify, limit_duration;
    uint64_t samples;
    station_fill_fn fill;
    void *ctx;
    struct stats *stats;
    int error;
} run_t;
static const char *stored;
static const char *station_store_get(void) { return stored; }
int probe_budget(const char *call, uint64_t budget, uint64_t *planned)
{
    stored = call;
    struct stats stats = {0};
    run_t value = {.identify = true, .limit_duration = true, .samples = budget, .stats = &stats};
    run_t *run = &value;
    *planned = 0;
''' + preparation + r'''
    *planned = run->samples;
finished:
    heap_caps_free(id);
    return run->error;
}
''')
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-shared",
                        "-fPIC", "-I", str(HEADER), str(SOURCE), str(probe), "-lm", "-o", str(libpath)], check=True)
        lib = C.CDLL(str(libpath))
        class Mark(C.Structure):
            _fields_ = [("start", C.c_uint32), ("end", C.c_uint32)]
        class Id(C.Structure):
            _fields_ = [("marks", Mark * 144), ("nmarks", C.c_uint), ("length", C.c_uint32),
                        ("payload_samples", C.c_uint64), ("total_samples", C.c_uint64),
                        ("payload", C.c_void_p), ("ctx", C.c_void_p), ("sine", C.c_int16 * 40),
                        ("cosine", C.c_int16 * 40), ("edge", C.c_uint8 * 501)]
        lib.probe_budget.argtypes = [C.c_char_p, C.c_uint64, C.POINTER(C.c_uint64)]
        for call in ("N0CALL", "W6/N0CALL/P", "A" + "0" * 22):
            for seconds in (1, 10, 11, 30, 60, 540, 550, 560, 600, 1200):
                planned = C.c_uint64()
                err = lib.probe_budget(call.encode(), seconds * RATE, C.byref(planned))
                if seconds <= 2 * morse.duration(call):
                    assert err != 0 and planned.value == 0
                else:
                    assert err == 0 and 0 < planned.value <= seconds * RATE
                    if seconds <= 60:
                        assert planned.value == seconds * RATE
        print("RF duration: actual preparation rejects too-short IDs and never exceeds the wall-time bound")
        u32p = C.POINTER(C.c_uint32)
        filltype = C.CFUNCTYPE(C.c_int, u32p, C.c_uint64, C.c_size_t, C.c_void_p)
        lib.station_id_init.argtypes = [C.POINTER(Id), C.c_char_p, C.c_uint64, C.c_void_p, C.c_void_p]
        lib.station_id_init.restype = C.c_bool
        for name in ("station_cw_fill", "station_id_fill"):
            getattr(lib, name).argtypes = [u32p, C.c_uint64, C.c_size_t, C.c_void_p]
        lib.station_callsign.argtypes = [C.c_char_p, C.c_char_p]
        lib.station_callsign.restype = C.c_bool
        lib.station_payload_header.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p]
        out = C.create_string_buffer(24)
        good = ["n0call", "W6/N0CALL/P", "k1a", "A" * 22 + "1"]
        for call in good:
            assert lib.station_callsign(call.encode(), out) and out.value.decode() == morse.callsign(call)
        for call in ["", "AB", "ABC", "123", "N0 CALL", 'N0"CALL', "N0CALL\\", "/N0CALL",
                     "N0CALL/", "N0//CALL", "é0CALL", "ß0CALL", "A" * 23 + "1", "N0CALL\n"]:
            assert not lib.station_callsign(call.encode(), out), call
            try:
                morse.callsign(call)
                raise AssertionError(call)
            except ValueError:
                pass

        calls = []
        @filltype
        def payload(dst, first, count, ctx):
            calls.append((first, count))
            arr = np.ctypeslib.as_array(dst, shape=(count,))
            arr[:] = np.arange(first, first + count, dtype=np.uint64).astype(np.uint32) ^ 0x12345678
            return 0

        def init(call="N0CALL", seconds=1200):
            id = Id()
            assert lib.station_id_init(C.byref(id), call.encode(), RATE * seconds, payload, None)
            return id

        def fill(id, first, n, cw=False):
            arr = np.empty(n, np.uint32)
            fn = lib.station_cw_fill if cw else lib.station_id_fill
            assert fn(arr.ctypes.data_as(u32p), first, n, C.byref(id)) == 0
            return arr

        def iq(words):
            i = ((words & 1023).astype(np.int32) + 512) % 1024 - 512
            q = (((words >> 10) & 1023).astype(np.int32) + 512) % 1024 - 512
            return i + 1j * q

        # Every allowed character, including slash; fixed hand-written timing
        # reference in Python checks C encoding independently of the decoder.
        for call in ["N0CALL", "W6/N0CALL/P", "ABCDEF1/GHIJKL", "MNOPQR2/STUVWX", "YZ34567890"]:
            id = init(call, 0)
            expected, at = [], 7
            for char in call:
                for mark in morse.CODES[char]:
                    end = at + (3 if mark == "-" else 1)
                    expected.append((at, end))
                    at = end + 1
                at += 2
            assert [(m.start, m.end) for m in id.marks[:id.nmarks]] == [
                (round(a * DOT), round(b * DOT)) for a, b in expected]
            assert id.length == round((at + 4) * DOT) == round(morse.duration(call) * RATE)
            # Exactly iddump's decimated actual C samples, without making a
            # multi-gigabyte array: call C for one sample at each output index.
            words = np.array([fill(id, n, 1, True)[0] for n in range(0, id.length, 4000)])
            result = morse.decode(iq(words), 10000, tone=0)
            assert result["text"] == call, result
        print("CW: every letter/digit/slash, 60 ms dots, 3-dot dashes, spacing, actual C dump decodes")

        id = init()
        capacity = PERIOD - id.length
        repeats = (id.payload_samples - 1) // capacity
        assert id.total_samples == id.payload_samples + (2 + repeats) * id.length
        assert id.length % 40 == 0 and id.length < RATE * 32
        # Seek straight to every transition, including >2^32 samples (107.4 s).
        boundaries = [0, id.length, PERIOD, PERIOD + id.length, 2 * PERIOD,
                      2 * PERIOD + id.length, id.total_samples - id.length, id.total_samples]
        for b in boundaries:
            first = max(0, b - 16000)
            direct = fill(id, first, 32760)
            split = np.concatenate([fill(id, first, 14268), fill(id, first + 14268, 18492)])
            assert np.array_equal(direct, split), b
            # Physical wrapped bank reads must produce identical lead/tail.
            bank_start = max(0, b // 14268 * 14268 - 64)
            old = fill(id, bank_start, 16380)
            new = fill(id, bank_start + 14268, 16380)
            assert np.array_equal(old[14268:], new[:2112]), b
        assert np.all(fill(id, id.total_samples, 16380) == 0)
        for cycle in range(3):
            start = cycle * PERIOD
            assert np.array_equal(fill(id, start, 16380), fill(id, 0, 16380))
            logical = cycle * capacity
            actual = fill(id, start + id.length, 16380)
            expected = np.arange(logical, logical + len(actual), dtype=np.uint64).astype(np.uint32) ^ 0x12345678
            assert np.array_equal(actual, expected)
        assert calls and all(n <= 32760 for _, n in calls)
        # Exact capacity and zero payload must still have complete start/end IDs.
        for samples in (0, capacity - 40, capacity, capacity + 40):
            exact = Id()
            assert lib.station_id_init(C.byref(exact), b"N0CALL", samples, payload, None)
            for b in (exact.length, exact.total_samples - exact.length):
                assert np.array_equal(fill(exact, b - 100, 1000), np.r_[
                    fill(exact, b - 100, 100), fill(exact, b, 900)])
        # Filler errors propagate without changing their meaning.
        @filltype
        def fail(dst, first, count, ctx):
            return 77
        broken = init()
        broken.payload = C.cast(fail, C.c_void_p).value
        arr = np.empty(100, np.uint32)
        assert lib.station_id_fill(arr.ctypes.data_as(u32p), broken.length, 100, C.byref(broken)) == 77
        print("schedule: start/end, 9-minute repeats, arbitrary seeks, overlap rereads, payload continuity, errors")

        for code in (4, 6, 11, 13, 17):  # QPSK and 8PSK frame sizes
            raw = bytearray(onair.payload(code, 3))
            per_frame = len(raw) // 3
            for f in range(3):
                arr = (C.c_uint8 * per_frame).from_buffer(raw, f * per_frame)
                lib.station_payload_header(arr, per_frame, b"N0CALL/P")
            assert raw == onair.payload(code, 3, "N0CALL/P")
            assert raw[:40] == b"ESP-DVBS2 ID=N0CALL/P".ljust(39) + b"\n"
            assert onair.stream_match(bytes(raw), bytes(raw[per_frame:]), per_frame) == per_frame
            wrong = onair.payload(code, 3, "N1CALL")
            assert onair.stream_match(wrong, bytes(raw), per_frame) is None
        print("payload: actual C header equals host reference; seeded bytes retain positions; wrong ID rejected")

        # Synthetic noisy capture, arbitrary tone phase/offset, scaled envelope.
        id = init("N0CALL", 0)
        words = np.array([fill(id, n, 1, True)[0] for n in range(0, id.length, 4000)])
        z = iq(words)
        rng = np.random.default_rng(2026)
        n = np.arange(len(z))
        capture = .3 * z * np.exp(1j * (.7 + 2 * np.pi * 1700 * n / 10000))
        capture += rng.normal(0, .5, len(z)) + 1j * rng.normal(0, .5, len(z))
        assert morse.decode(capture, 10000, 1700)["text"] == "N0CALL"
        for bad in (np.zeros(10000), np.ones(10000), rng.normal(size=10000), np.full(10000, np.nan)):
            try:
                morse.decode(bad, 10000, 0)
                raise AssertionError("bad IQ accepted")
            except ValueError:
                pass
        path = tmp / "id.iq"
        np.column_stack((capture.real, capture.imag)).astype("<f4").tofile(path)
        command = [sys.executable, str(ROOT / "host/morse.py"), str(path), "--rate", "10000",
                   "--tone", "1700", "--format", "f32", "--expect", "N0CALL"]
        assert json.loads(subprocess.check_output(command))["match"]
        assert subprocess.run(command[:-1] + ["N1CALL"], capture_output=True).returncode == 1
        print("decoder: noisy synthetic capture, CLI, phase/frequency offsets; wrong ID, silence/noise rejected")

        if "--spectrum" in sys.argv:
            # Numerical spectrum of actual packed C DAC words for one isolated
            # 60 ms dot, rectangular FFT window padded by 10 ms silence/side.
            id = init("E0A", 0)
            mark = id.marks[0]
            first, count = mark.start - 400000, mark.end - mark.start + 800000
            shaped = iq(fill(id, first, count, True))
            tone = iq(fill(id, mark.start + 400000, 40, True))
            hard = np.zeros(count, complex)
            hard[400000:-400000] = np.tile(tone, (mark.end - mark.start) // 40)
            freq = np.fft.fftfreq(1 << (count - 1).bit_length(), 1 / RATE)
            distance = abs(freq - 1e6)
            record = {"evidence": "host numerical spectrum of actual C samples, no RF",
                      "sample_rate": RATE, "dot_seconds": .06, "edge_seconds": .005,
                      "fft_samples": len(freq), "sample_count": count}
            for label, z in (("shaped", shaped), ("rectangular", hard)):
                power = abs(np.fft.fft(z, len(freq)))**2
                for limit in (1000, 5000):
                    record[f"{label}_outside_{limit}_hz_dbc"] = float(10 * np.log10(
                        power[distance > limit].sum() / power.sum()))
                    record[f"{label}_{limit}_to_50000_hz_dbc"] = float(10 * np.log10(
                        power[(distance > limit) & (distance < 50000)].sum() / power.sum()))
            print(json.dumps(record, sort_keys=True))



if __name__ == "__main__":
    run()
