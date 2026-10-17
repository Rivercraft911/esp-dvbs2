"""Compile the portable C encoder with cc and compare every stage with the model.

Run: python3 tests/test_dvbs2_c.py. No ESP-IDF, board, or pytest needed.
"""
import ctypes as ct
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2 as d

MODES = [(k, "qpsk", r) for k, r in enumerate(d.RATES[3:], 4)]
MODES += [(k, "8psk", r) for k, r in enumerate(d.PSK8_RATES, 12)]
U8P = ct.POINTER(ct.c_uint8)


class NativeEncoder:
    def __enter__(self):
        subprocess.run([sys.executable, str(ROOT / "tests/reference/gen_dvbs2_c.py"), "--check"],
                       check=True)
        subprocess.run([sys.executable, str(ROOT / "tests/reference/gen_dvbs2_fast.py"), "--check"], check=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="dvbs2-c-")
        folder = pathlib.Path(self.tmp.name)
        bridge = folder / "sizes.c"
        bridge.write_text('#include "dvbs2.h"\n'
                          'size_t workspace_size(void) { return sizeof(dvbs2_workspace); }\n'
                          'size_t state_size(void) { return sizeof(dvbs2_ts_state); }\n')
        library = folder / "dvbs2.so"
        component = ROOT / "firmware/components/dvbs2"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            "-I", str(component / "include"), str(component / "dvbs2.c"),
            str(component / "fast.c"), str(bridge), "-o", str(library)], check=True)
        self.lib = ct.CDLL(str(library))
        api = {
            "workspace_size": (ct.c_size_t, []),
            "state_size": (ct.c_size_t, []),
            "dvbs2_find_mode": (ct.c_void_p, [ct.c_uint]),
            "dvbs2_frame_symbols": (ct.c_size_t, [ct.c_void_p, ct.c_int]),
            "dvbs2_bb_gcs": (ct.c_int, [ct.c_void_p, ct.c_void_p, U8P, ct.c_size_t]),
            "dvbs2_bb_gcs_ro": (ct.c_int, [ct.c_void_p, ct.c_void_p, U8P, ct.c_size_t, ct.c_uint]),
            "dvbs2_bb_ts": (ct.c_int, [ct.c_void_p, ct.c_void_p, ct.c_void_p,
                                      U8P, ct.c_size_t]),
            "dvbs2_bb_scramble": (None, [ct.c_void_p, ct.c_void_p]),
            "dvbs2_bch": (None, [ct.c_void_p, ct.c_void_p]),
            "dvbs2_ldpc": (None, [ct.c_void_p, ct.c_void_p]),
            "dvbs2_map": (None, [ct.c_void_p, ct.c_void_p, U8P]),
            "dvbs2_plframe": (None, [ct.c_void_p, ct.c_int, U8P]),
            "dvbs2_fast_size": (ct.c_size_t, []),
            "dvbs2_fast_mode_size": (ct.c_size_t, [ct.c_void_p]),
            "dvbs2_encode_frame": (ct.c_int, [ct.c_void_p, ct.c_void_p, ct.c_void_p,
                                              U8P, ct.c_size_t, U8P]),
            "dvbs2_fast_prepare": (ct.c_int, [ct.c_void_p, ct.c_void_p, ct.c_int, ct.c_uint]),
            "dvbs2_fast_cache_bch": (ct.c_int, [ct.c_void_p, U8P]),
            "dvbs2_fast_pie": (ct.c_uint, [ct.c_void_p]),
            "dvbs2_fast_bb_gcs": (ct.c_int, [ct.c_void_p, ct.c_void_p, U8P, ct.c_size_t]),
            "dvbs2_fast_bb_ts": (ct.c_int, [ct.c_void_p, ct.c_void_p, ct.c_void_p,
                                           U8P, ct.c_size_t]),
            "dvbs2_fast_bch": (None, [ct.c_void_p, ct.c_void_p]),
            "dvbs2_fast_ldpc": (None, [ct.c_void_p, ct.c_void_p]),
            "dvbs2_fast_map": (None, [ct.c_void_p, ct.c_void_p, U8P]),
            "dvbs2_fast_pl": (None, [ct.c_void_p, U8P]),
            "dvbs2_fast_map_packed": (ct.c_int, [ct.c_void_p, ct.c_void_p, U8P]),
            "dvbs2_fast_pl_packed": (None, [ct.c_void_p, U8P]),
            "dvbs2_encode_frame_packed": (ct.c_int, [ct.c_void_p, ct.c_void_p, ct.c_void_p,
                                                     U8P, ct.c_size_t, U8P]),
            "dvbs2_pack": (None, [U8P, U8P, ct.c_size_t]),
            "dvbs2_dummy": (None, [U8P]),
        }
        for name, (result, arguments) in api.items():
            fn = getattr(self.lib, name)
            fn.restype, fn.argtypes = result, arguments
        self.ws = ct.create_string_buffer(self.lib.workspace_size())
        # 16-byte aligned fast context inside an oversized buffer.
        self.fast_raw = ct.create_string_buffer(self.lib.dvbs2_fast_size() + 16)
        self.fast = ct.addressof(self.fast_raw) + (-ct.addressof(self.fast_raw) % 16)
        return self

    def __exit__(self, *args):
        self.tmp.cleanup()

    def bits(self, length):
        return np.unpackbits(np.frombuffer(self.ws, np.uint8, count=8100))[:length]

    def load(self, bits):
        packed = np.packbits(bits)
        ct.memmove(self.ws, packed.ctypes.data, len(packed))

    def stages(self, bbf, mode, rate, modulation, pilots):
        c = d.code(rate)
        np.testing.assert_array_equal(self.bits(c.kbch), bbf, err_msg="BBFRAME")
        self.lib.dvbs2_bb_scramble(self.ws, mode)
        scr = d.bb_scramble(bbf)
        np.testing.assert_array_equal(self.bits(c.kbch), scr, err_msg="BB scrambling")
        self.lib.dvbs2_bch(self.ws, mode)
        bch = d.bch_encode(scr, rate)
        np.testing.assert_array_equal(self.bits(c.nbch), bch, err_msg="BCH")
        self.lib.dvbs2_ldpc(self.ws, mode)
        fec = d.ldpc_encode_groups(bch, rate)
        np.testing.assert_array_equal(self.bits(c.n), fec, err_msg="LDPC")
        self.phases(fec, mode, rate, modulation, pilots)

    def phases(self, fec, mode, rate, modulation, pilots):
        count = self.lib.dvbs2_frame_symbols(mode, pilots)
        assert count == d.plframe_length(False, pilots, modulation)
        # Guard both ends, including the exact no-pilot output allocation.
        raw = np.full(count + 32, 0xA5, np.uint8)
        output = raw[16:-16]
        self.lib.dvbs2_map(self.ws, mode, output[90:].ctypes.data_as(U8P))
        if modulation == "8psk":
            mapped = d.psk8_map(d.bit_interleave(fec, rate))
        else:
            bi, bq = d.qpsk_map(fec)
            mapped = np.array([1, 7, 3, 5], np.uint8)[2 * bi + bq]
        np.testing.assert_array_equal(output[90:90 + len(mapped)], mapped,
                                      err_msg="interleaver / mapping")
        self.lib.dvbs2_plframe(mode, pilots, output.ctypes.data_as(U8P))
        expected = d.plframe_phases(fec, rate, pilots=pilots, modulation=modulation)
        np.testing.assert_array_equal(output, expected, err_msg="PLFRAME")
        assert np.all(raw[:16] == 0xA5) and np.all(raw[-16:] == 0xA5)
        return output.copy()


    def fast_stages(self, bbf, mode, rate, modulation, pilots, offset=0):
        """Fast path after fast_bb_*: every stage against the model and the
        PLFRAME written at an arbitrary output offset with guard bytes."""
        c = d.code(rate)
        scr = d.bb_scramble(bbf)
        np.testing.assert_array_equal(self.bits(c.kbch), scr, err_msg="fast BB")
        self.lib.dvbs2_fast_bch(self.fast, self.ws)
        bch = d.bch_encode(scr, rate)
        np.testing.assert_array_equal(self.bits(c.nbch), bch, err_msg="fast BCH")
        self.lib.dvbs2_fast_ldpc(self.fast, self.ws)
        fec = d.ldpc_encode_groups(bch, rate)
        np.testing.assert_array_equal(self.bits(c.n), fec, err_msg="fast LDPC")
        count = self.lib.dvbs2_frame_symbols(mode, pilots)
        raw = np.full(count + 64, 0xA5, np.uint8)
        output = raw[16 + offset:16 + offset + count]
        self.lib.dvbs2_fast_map(self.fast, self.ws, output.ctypes.data_as(U8P))
        self.lib.dvbs2_fast_pl(self.fast, output.ctypes.data_as(U8P))
        expected = d.plframe_phases(fec, rate, pilots=pilots, modulation=modulation)
        np.testing.assert_array_equal(output, expected, err_msg="fast PLFRAME")
        assert np.all(raw[:16 + offset] == 0xA5) and np.all(raw[16 + offset + count:] == 0xA5)


def test_modes(native):
    assert not native.lib.dvbs2_find_mode(0)
    for k in list(range(1, 4)) + list(range(18, 33)):
        assert not native.lib.dvbs2_find_mode(k)
    for modcod, modulation, rate in MODES:
        assert native.lib.dvbs2_find_mode(modcod)
        assert d.modcod(rate, modulation) == modcod


def test_gcs(native):
    rng = np.random.default_rng(20261016)
    frames = 0
    for modcod, modulation, rate in MODES:
        mode, c = native.lib.dvbs2_find_mode(modcod), d.code(rate)
        capacity = c.kbch - 80
        for case, length in enumerate((0, 1, 7, 8, 9, capacity - 1, capacity, capacity)):
            bits = rng.integers(0, 2, length, dtype=np.uint8)
            if case == 2:
                bits.fill(1)
            if case == 6:
                bits.fill(0)
            if case == 7:
                bits.fill(1)
            packed = np.packbits(bits)
            if length % 8:
                packed[-1] |= (1 << (8 - length % 8)) - 1
            assert native.lib.dvbs2_bb_gcs(native.ws, mode, packed.ctypes.data_as(U8P),
                                           length) == 0
            native.stages(d.bbframe(bits, rate), mode, rate, modulation, case % 2)
            frames += 1
    print(f"C GCS: {frames} frames, all 14 modes, every stage, empty/partial/full fields, "
          "both pilot settings, zero/one/random payloads")


def test_rolloff(native):
    for mc, modulation, rate in MODES:
        mode, c = native.lib.dvbs2_find_mode(mc), d.code(rate)
        bits = np.random.default_rng(mc).integers(0, 2, c.kbch-80, dtype=np.uint8)
        # Keep the payload alive while C reads it.
        packed = np.packbits(bits)
        for ro, alpha in enumerate((.35, .25, .20)):
            assert native.lib.dvbs2_bb_gcs_ro(native.ws, mode, packed.ctypes.data_as(U8P), len(bits), ro) == 0
            native.stages(d.bbframe(bits, rate, ro=alpha), mode, rate, modulation, 1)
        assert native.lib.dvbs2_fast_prepare(native.fast, mode, 1, 15 | 1024) == 0
        assert native.lib.dvbs2_fast_bb_gcs(native.fast, native.ws, packed.ctypes.data_as(U8P), len(bits)) == 0
        native.fast_stages(d.bbframe(bits, rate, ro=.20), mode, rate, modulation, 1)
        before = bytes(native.ws)
        assert native.lib.dvbs2_bb_gcs_ro(native.ws, mode, packed.ctypes.data_as(U8P), len(bits), 3) == -1
        assert bytes(native.ws) == before
    print("roll-off: plain .35/.25/.20 and fast .20, every stage against Python, invalid code leaves workspace untouched")


def test_ts(native):
    rng, frames = np.random.default_rng(932711), 0
    for modcod, modulation, rate in MODES:
        mode, c = native.lib.dvbs2_find_mode(modcod), d.code(rate)
        size, count = (c.kbch - 80) // 8, 5
        packets = rng.integers(0, 256, ((count * size + 187) // 188, 188), dtype=np.uint8)
        packets[:, 0] = 0x47
        expected = d.ts_bbframes(packets, rate)
        stream = packets.ravel()
        state = ct.create_string_buffer(native.lib.state_size())
        for k in range(count):
            payload = stream[k * size:(k + 1) * size]
            assert native.lib.dvbs2_bb_ts(native.ws, mode, state,
                                          payload.ctypes.data_as(U8P), size) == 0
            native.stages(expected[k], mode, rate, modulation, k % 2)
            frames += 1
    print(f"C TS: {frames} consecutive frames, all 14 modes, every stage; packet splitting, "
          "SYNCD and previous-packet CRC state across frames")


def test_ldpc_boundaries(native):
    # Isolate the packed word/360-bit wrap cases with single information bits.
    # Compare with the standard's bit-address formulation, not the group model.
    for modcod, rate in ((4, "1/2"), (6, "2/3"), (11, "9/10")):
        mode, c = native.lib.dvbs2_find_mode(modcod), d.code(rate)
        for bit in (0, 1, 7, 8, 31, 32, 351, 352, 359, 360, c.nbch - 1):
            message = np.zeros(c.nbch, np.uint8)
            message[bit] = 1
            native.load(message)
            native.lib.dvbs2_ldpc(native.ws, mode)
            np.testing.assert_array_equal(native.bits(c.n), d.ldpc_encode(message, rate),
                                          err_msg=f"LDPC {rate} impulse {bit}")
    print("C LDPC: 33 impulse codewords match bit-address equations at byte, word, "
          "360-bit group and final-bit boundaries")


def test_invalid_inputs(native):
    mode = native.lib.dvbs2_find_mode(4)
    cap = (d.code("1/2").kbch - 80) // 8
    data = np.zeros(cap, np.uint8)
    data[::188] = 0x47
    state = ct.create_string_buffer(native.lib.state_size())
    before = bytes(native.ws)
    assert native.lib.dvbs2_bb_gcs(native.ws, mode, data.ctypes.data_as(U8P),
                                  cap * 8 + 1) == -1
    assert bytes(native.ws) == before
    assert native.lib.dvbs2_bb_gcs(native.ws, mode, None, 1) == -1
    assert bytes(native.ws) == before
    for length in (0, cap - 1, cap + 1):
        assert native.lib.dvbs2_bb_ts(native.ws, mode, state,
                                     data.ctypes.data_as(U8P), length) == -1
        assert bytes(native.ws) == before and not any(bytes(state))
    data[188] = 0
    assert native.lib.dvbs2_bb_ts(native.ws, mode, state,
                                 data.ctypes.data_as(U8P), cap) == -1
    assert bytes(native.ws) == before and not any(bytes(state))
    data[188] = 0x47
    assert native.lib.dvbs2_bb_ts(native.ws, mode, state,
                                 data.ctypes.data_as(U8P), cap) == 0
    before, state_before = bytes(native.ws), bytes(state)
    # Continue mid-packet and corrupt the first sync within the next slice.
    continued = np.zeros(cap, np.uint8)
    next_sync = (-cap) % 188
    continued[next_sync::188] = 0x47
    continued[next_sync] = 0
    assert native.lib.dvbs2_bb_ts(native.ws, mode, state,
                                 continued.ctypes.data_as(U8P), cap) == -1
    assert bytes(native.ws) == before and bytes(state) == state_before
    print("C input validation: invalid GCS lengths/pointers and TS lengths/sync rejected "
          "without modifying frame or stream state")


def test_fast(native):
    """Fast C path: GCS and TS frames in all modes, both pilot settings, PLFRAME
    at 16 output alignments, LDPC impulses, and the same input checks."""
    lib, fast = native.lib, native.fast
    rng, frames = np.random.default_rng(1729), 0
    for modcod, modulation, rate in MODES:
        mode, c = lib.dvbs2_find_mode(modcod), d.code(rate)
        capacity = c.kbch - 80
        for pilots in (1, 0):
            assert lib.dvbs2_fast_prepare(fast, mode, pilots, 0) == 0
            assert lib.dvbs2_fast_pie(fast) == 0
            for case, length in enumerate((capacity, 0, 13, capacity - 3)):
                bits = rng.integers(0, 2, length, dtype=np.uint8)
                packed = np.packbits(bits)
                if length % 8:
                    packed[-1] |= (1 << (8 - length % 8)) - 1
                assert lib.dvbs2_fast_bb_gcs(fast, native.ws, packed.ctypes.data_as(U8P),
                                             length) == 0
                native.fast_stages(d.bbframe(bits, rate), mode, rate, modulation, pilots,
                                   offset=(5 * frames) % 16)
                frames += 1
        assert lib.dvbs2_fast_prepare(fast, mode, 1, 0) == 0
        size, count = (c.kbch - 80) // 8, 3
        packets = rng.integers(0, 256, ((count * size + 187) // 188, 188), dtype=np.uint8)
        packets[:, 0] = 0x47
        expected = d.ts_bbframes(packets, rate)
        stream = packets.ravel()
        state = ct.create_string_buffer(lib.state_size())
        for k in range(count):
            payload = stream[k * size:(k + 1) * size]
            assert lib.dvbs2_fast_bb_ts(fast, native.ws, state,
                                        payload.ctypes.data_as(U8P), size) == 0
            native.fast_stages(expected[k], mode, rate, modulation, 1, offset=k)
            frames += 1
    for modcod, rate in ((4, "1/2"), (13, "2/3"), (11, "9/10")):
        mode, c = lib.dvbs2_find_mode(modcod), d.code(rate)
        assert lib.dvbs2_fast_prepare(fast, mode, 1, 0) == 0
        for bit in (0, 1, 7, 8, 31, 32, 351, 352, 359, 360, c.nbch - 1):
            message = np.zeros(c.nbch, np.uint8)
            message[bit] = 1
            native.load(message)
            lib.dvbs2_fast_ldpc(fast, native.ws)
            np.testing.assert_array_equal(native.bits(c.n), d.ldpc_encode(message, rate),
                                          err_msg=f"fast LDPC {rate} impulse {bit}")
    mode = lib.dvbs2_find_mode(4)
    assert lib.dvbs2_fast_prepare(fast, mode, 1, 0) == 0
    cap = (d.code("1/2").kbch - 80) // 8
    data = np.zeros(cap, np.uint8)
    data[::188] = 0x47
    state = ct.create_string_buffer(lib.state_size())
    before = bytes(native.ws)
    assert lib.dvbs2_fast_bb_gcs(fast, native.ws, data.ctypes.data_as(U8P), cap * 8 + 1) == -1
    for length in (cap - 1, cap + 1):
        assert lib.dvbs2_fast_bb_ts(fast, native.ws, state, data.ctypes.data_as(U8P),
                                    length) == -1
    data[376] = 0
    assert lib.dvbs2_fast_bb_ts(fast, native.ws, state, data.ctypes.data_as(U8P), cap) == -1
    assert bytes(native.ws) == before and not any(bytes(state))
    assert lib.dvbs2_fast_prepare(fast, None, 1, 0) == -1
    print(f"C fast path: {frames} GCS/TS frames, all 14 modes, pilots on/off, every stage "
          "and the PLFRAME at shifted output addresses; 33 LDPC impulses; input checks")



def test_complete_frame(native):
    lib = native.lib
    for modcod, modulation, rate in MODES:
        mode = lib.dvbs2_find_mode(modcod)
        size = lib.dvbs2_fast_mode_size(mode)
        assert 0 < size <= lib.dvbs2_fast_size() < 22000
        n = (d.code(rate).kbch - 80) // 8
        payload = np.random.default_rng(modcod).integers(0, 256, n, dtype=np.uint8)
        for pilots in (0, 1):
            # Allocate only this mode's advertised size and guard both ends.
            raw = np.full(size + 64, 0xA5, np.uint8)
            at = 16 + (-raw.ctypes.data % 16)
            f = raw.ctypes.data + at
            assert lib.dvbs2_fast_prepare(f, mode, pilots, 15) == 0
            count = lib.dvbs2_frame_symbols(mode, pilots)
            output = np.full(count + 32, 0x5A, np.uint8)
            dst = output[16:-16].ctypes.data_as(U8P)
            bits = np.unpackbits(payload)[:-3]
            assert lib.dvbs2_encode_frame(f, native.ws, None, payload.ctypes.data_as(U8P), len(bits), dst) == 0
            expected = d.plframe_phases(d.fecframe(d.bbframe(bits, rate), rate), rate,
                                        pilots=pilots, modulation=modulation)
            np.testing.assert_array_equal(output[16:-16], expected)
            assert np.all(raw[:at] == 0xA5) and np.all(raw[at + size:] == 0xA5)
            assert np.all(output[:16] == 0x5A) and np.all(output[-16:] == 0x5A)
            # TS continuation through the complete API agrees with the original stages.
            state = ct.create_string_buffer(lib.state_size())
            refstate = ct.create_string_buffer(lib.state_size())
            for frame in range(2):
                stream = payload.copy()
                stream[np.arange(n)[(np.arange(n) + frame * n) % 188 == 0]] = 0x47
                src = stream.ctypes.data_as(U8P)
                assert lib.dvbs2_bb_ts(native.ws, mode, refstate, src, n) == 0
                bbf = native.bits(d.code(rate).kbch).copy()
                expected = d.plframe_phases(d.fecframe(bbf, rate), rate, pilots=pilots, modulation=modulation)
                assert lib.dvbs2_encode_frame(f, native.ws, state, src, 8*n, dst) == 0
                np.testing.assert_array_equal(output[16:-16], expected)
                assert state.raw == refstate.raw
            saved = native.ws.raw, state.raw, output.copy()
            for args in ((f, native.ws, state, src, 8*n - 1, dst),
                         (f, native.ws, None, src, 8*n + 1, dst),
                         (f, native.ws, None, src, 8*n, None),
                         (None, native.ws, None, src, 8*n, dst)):
                assert lib.dvbs2_encode_frame(*args) == -1
                assert native.ws.raw == saved[0] and state.raw == saved[1]
                np.testing.assert_array_equal(output, saved[2])
    assert lib.dvbs2_fast_mode_size(None) == 0
    print("complete fast API: all modes, both pilots, GCS/TS continuation, exact-sized contexts and invalid-input guards")

def pack(phases):
    phases = np.asarray(phases, np.uint8)
    return (phases[0::2] | phases[1::2] << 4).astype(np.uint8)


def test_packed(native):
    """Packed QPSK/8PSK frames (two phases per byte) equal the model's phases, packed."""
    lib = native.lib
    frames = 0
    for modcod, modulation, rate in MODES:
        mode = lib.dvbs2_find_mode(modcod)
        size = lib.dvbs2_fast_mode_size(mode)
        n = (d.code(rate).kbch - 80) // 8
        raw = np.full(size + 64, 0xA5, np.uint8)
        at = 16 + (-raw.ctypes.data % 16)
        f = raw.ctypes.data + at
        rng = np.random.default_rng(100 + modcod)
        for pilots in (0, 1):
            assert lib.dvbs2_fast_prepare(f, mode, pilots, 15) == 0
            count = lib.dvbs2_frame_symbols(mode, pilots)
            assert count % 2 == 0
            state = ct.create_string_buffer(lib.state_size())
            for frame, offset in enumerate((0, 1, 2, 3, 7)):
                payload = rng.integers(0, 256, n, dtype=np.uint8)
                bits = np.unpackbits(payload)[:8 * n - 5 * (frame == 0)]
                output = np.full(count // 2 + 32, 0x5A, np.uint8)
                dst = output[16 + offset:].ctypes.data_as(U8P)
                ts = frame >= 3
                if ts:
                    payload[np.arange(n)[(np.arange(n) + (frame - 3) * n) % 188 == 0]] = 0x47
                    ref = ct.create_string_buffer(lib.state_size())
                    ref.raw = state.raw
                    assert lib.dvbs2_bb_ts(native.ws, mode, ref, payload.ctypes.data_as(U8P), n) == 0
                    bbf = native.bits(d.code(rate).kbch).copy()
                    assert lib.dvbs2_encode_frame_packed(f, native.ws, state,
                                                         payload.ctypes.data_as(U8P), 8 * n, dst) == 0
                    assert state.raw == ref.raw
                else:
                    bbf = d.bbframe(bits, rate)
                    assert lib.dvbs2_encode_frame_packed(f, native.ws, None,
                                                         payload.ctypes.data_as(U8P), len(bits), dst) == 0
                expected = d.plframe_phases(d.fecframe(bbf, rate), rate, pilots=pilots,
                                            modulation=modulation)
                body = output[16 + offset:16 + offset + count // 2]
                np.testing.assert_array_equal(body, pack(expected))
                assert np.all(output[:16 + offset] == 0x5A)
                assert np.all(output[16 + offset + count // 2:] == 0x5A)
                assert np.all(raw[:at] == 0xA5) and np.all(raw[at + size:] == 0xA5)
                frames += 1
            # The stages alone match the complete call.
            again = np.zeros(count // 2, np.uint8)
            assert lib.dvbs2_fast_map_packed(f, native.ws, again.ctypes.data_as(U8P)) == 0
            lib.dvbs2_fast_pl_packed(f, again.ctypes.data_as(U8P))
            np.testing.assert_array_equal(again, body)
    dummy = np.zeros(3330, np.uint8)
    lib.dvbs2_dummy(dummy.ctypes.data_as(U8P))
    packed = np.zeros(1665, np.uint8)
    lib.dvbs2_pack(dummy.ctypes.data_as(U8P), packed.ctypes.data_as(U8P), 3330)
    np.testing.assert_array_equal(packed, pack(dummy))
    assert lib.dvbs2_encode_frame_packed(None, native.ws, None, packed.ctypes.data_as(U8P), 8,
                                         packed.ctypes.data_as(U8P)) == -1
    print(f"packed frames: {frames} GCS/TS frames, all 14 MODCODs, pilots on/off, five output "
          "alignments; equal to the model's phases packed two per byte")


def test_bch_cache(native):
    lib = native.lib
    cache = np.full(8192 + 64, 0xA5, np.uint8)
    at = 16 + (-cache.ctypes.data % 16)
    table = cache[at:at + 8192].ctypes.data_as(U8P)
    assert lib.dvbs2_fast_cache_bch(None, table) == -1
    for modcod, modulation, rate in MODES:
        mode, c = lib.dvbs2_find_mode(modcod), d.code(rate)
        assert lib.dvbs2_fast_prepare(native.fast, mode, 1, 15) == 0
        assert lib.dvbs2_fast_cache_bch(native.fast, cache[at + 1:].ctypes.data_as(U8P)) == -1
        if c.t == 8:
            assert lib.dvbs2_fast_cache_bch(native.fast, table) == -1
            continue
        data = np.random.default_rng(modcod).integers(0, 2, c.kbch, dtype=np.uint8)
        native.load(data)
        assert lib.dvbs2_fast_cache_bch(native.fast, table) == 0
        lib.dvbs2_fast_bch(native.fast, native.ws)
        np.testing.assert_array_equal(native.bits(c.nbch), d.bch_encode(data, rate))
        saved = cache.copy()
        assert lib.dvbs2_fast_cache_bch(native.fast, None) == 0
        lib.dvbs2_fast_bch(native.fast, native.ws)
        np.testing.assert_array_equal(native.bits(c.nbch), d.bch_encode(data, rate))
        np.testing.assert_array_equal(cache, saved)
        assert np.all(cache[:at] == 0xA5) and np.all(cache[at + 8192:] == 0xA5)
    print("resident BCH: every t=10/12 mode, canonical parity, guards, restore and invalid alignment")


if __name__ == "__main__":
    with NativeEncoder() as native:
        for test in (test_modes, test_gcs, test_ts, test_ldpc_boundaries, test_invalid_inputs,
                     test_rolloff, test_fast, test_complete_frame, test_packed, test_bch_cache):
            test(native)
