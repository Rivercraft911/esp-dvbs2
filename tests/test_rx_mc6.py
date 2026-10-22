"""MC6 layered decoder: exact states, external codewords and noisy held-outs.

Run python3 tests/test_rx_mc6.py. Host correctness only; no native cycle claim.
The temporary executable is independently checked under ASan/UBSan. Expected
words/payloads are used only after the real decoder has finished.
"""
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2 as d
import rx_mc6_vectors as oracle
from dvbs2_tables import BCH_POLYS


def graph_input():
    rows = d.code("2/3").rows
    return bytes(map(len, rows)) + b"".join(struct.pack("<H", int(a)) for row in rows for a in row)


def run(exe, mode, iterations, llr, graph=False):
    data = (graph_input() if graph else b"") + np.asarray(llr, dtype=np.int8).tobytes()
    proc = subprocess.run([str(exe), mode, str(iterations)], input=data,
                          capture_output=True, check=True, timeout=120)
    return proc.stdout, json.loads(proc.stderr)


def fixture(seed, length=d.code("2/3").kbch - 80):
    rng = np.random.default_rng(seed)
    payload = rng.integers(0, 2, length, dtype=np.uint8)
    bb = d.bbframe(payload, "2/3")
    word = d.fecframe(bb, "2/3")
    assert not oracle.syndrome(word).any()
    return word, np.packbits(payload).tobytes()


def check_result(exe, word, expected_payload, llr, iterations=8):
    data, result = run(exe, "decode", iterations, llr)
    for mode in ("packed0", "packed1"):
        packed_data, packed_result = run(exe, mode, iterations, llr)
        assert packed_data == data and packed_result == result, (mode, result, packed_result)
    got = np.unpackbits(np.frombuffer(data[:8100], np.uint8))
    assert oracle.syndrome(got).sum() == result["H"]
    if result["valid"]:
        assert result["H"] == 0 and result["bch"] and result["header"]
        assert np.array_equal(got, word), result
        assert data[8100:] == expected_payload
        assert result["bytes"] == len(expected_payload)
    else:
        assert len(data) == 8100 and result["bytes"] == 0
    return result


def test():
    oracle.self_check()
    # Construct g from the standard factors, then long-divide every table entry.
    # This uses neither production nibble entries nor the host byte-table helper.
    generator = 1
    for terms in BCH_POLYS["normal"][:10]:
        factor = sum(1 << n for n in terms)
        product = 0
        while factor:
            if factor & 1:
                product ^= generator
            generator <<= 1
            factor >>= 1
        generator = product
    assert generator.bit_length() == 161
    table = (ROOT / "firmware/rx/fec/mc6_bch8.h").read_text()
    stored = [int(n, 16) for n in re.findall(r"UINT32_C\(0x([0-9a-f]+)\)", table)]
    expected_table = []
    for value in range(256):
        remainder = value << 160
        while remainder.bit_length() > 160:
            remainder ^= generator << (remainder.bit_length() - 161)
        expected_table.extend((remainder >> (32 * n)) & 0xffffffff for n in range(5))
    assert stored == expected_table
    with tempfile.TemporaryDirectory(prefix="rx-mc6-") as tmp:
        exe = pathlib.Path(tmp) / "mc6"
        command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DRX_MC6_TEST_HOOKS",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I" + str(ROOT / "firmware/rx/fec"),
                   str(ROOT / "tests/rx_mc6.c"), str(ROOT / "firmware/rx/fec/rx_mc6.c"),
                   "-o", str(exe)]
        subprocess.run(command, check=True)
        # Leak checking is unavailable on some macOS ASan runtimes; allocation
        # ownership is explicit and every worker frees its own buffers.
        os.environ["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        subprocess.run([str(exe)], check=True)
        # Exercise the real full-word checker on arbitrary inputs: retaining
        # only the zero-syndrome predicate would miss tail/popcount mistakes.
        checker_rng = np.random.default_rng(0x6C360)
        packed_words = [bytes(8100), bytes([255]) * 8100, bytes([0x55]) * 8100,
                        bytes([0xaa]) * 8100]
        packed_words += [checker_rng.integers(0, 256, 8100, dtype=np.uint8).tobytes()
                         for _ in range(32)]
        for packed in packed_words:
            checked = subprocess.run([str(exe), "check"], input=packed,
                                     capture_output=True, check=True, timeout=30)
            result = json.loads(checked.stdout)
            expected = int(oracle.syndrome(np.unpackbits(np.frombuffer(packed, np.uint8))).sum())
            assert result["H"] == expected, (result, expected)
            polynomial = int.from_bytes(packed[:5400], "big")
            while polynomial.bit_length() > 160:
                polynomial ^= generator << (polynomial.bit_length() - 161)
            assert result["bch"] == (polynomial == 0)
        # All 64800 natural variable positions: every byte, 32-bit chunk,
        # circular 360-bit row, QC/parity transition and final-bit boundary.
        basis = subprocess.run([str(exe), "basis", "64800"], capture_output=True,
                               check=True, timeout=300)
        got = np.frombuffer(basis.stdout, dtype="<u2")
        expected = np.array([len(oracle.natural_checks(n)) for n in range(64800)], dtype=np.uint16)
        assert np.array_equal(got, expected)
        print("MC6 QC checker: exact H on 36 arbitrary words and all 64800 natural bit columns PASS", flush=True)
        for word, bb in oracle.crosschecked_frames():
            llr = (1 - 2 * word.astype(np.int16)) * 24
            result = check_result(exe, word, np.packbits(bb[80:]).tobytes(), llr)
            assert result["valid"] and result["iterations"] == 0

        word, payload = fixture(0x622026)
        rng = np.random.default_rng(0x6A8319)
        signs = 1 - 2 * word.astype(np.int16)
        # Every tile compares all beliefs and all compressed states; every
        # touched message is reconstructed against independent signed R edges.
        # Full-range channel input also exercises the int16 belief invariant.
        for llr in (rng.integers(-128, 128, 64800, dtype=np.int16), signs * 24):
            for mode in ("state", "blocked_state"):
                _, state = run(exe, mode, 2, llr, graph=True)
                assert state == {"tiles": 5490, "iterations": 2,
                                 "all_beliefs_messages_per_tile": True}

        clean = check_result(exe, word, payload, signs * 24)
        assert clean["valid"] and not clean["iterations"]
        partial, small_payload = fixture(0x6101, 8000)
        assert check_result(exe, partial, small_payload,
                            (1 - 2 * partial.astype(np.int16)) * 24)["valid"]

        for position in (0, 31, 32, 159, 160, 43039, 43040, 43199, 43200, 43260, 64799):
            corrupted = word.copy()
            corrupted[position] ^= 1
            result = check_result(exe, word, payload,
                                  (1 - 2 * corrupted.astype(np.int16)) * 24, iterations=0)
            assert not result["valid"] and result["H"]
            assert result["bch"] == (position >= 43200)

        # Re-encoding deliberately invalid headers keeps BOTH FEC layers valid:
        # a CRC-only or fixed-length header shortcut must not publish the data.
        plain = d.bbframe(np.unpackbits(np.frombuffer(payload, np.uint8)), "2/3")
        headers = [d.bbheader(d.matype1(d.GCS), 42968),
                   d.bbheader(d.matype1(d.GCS), 42960, upl=8),
                   d.bbheader(d.matype1(d.GCS), 42960, syncd=8),
                   d.bbheader(d.matype1(d.GCS), 42960, mt2=1)]
        bad_crc = plain[:80].copy()
        bad_crc[79] ^= 1
        headers.append(bad_crc)
        for header in headers:
            altered = plain.copy()
            altered[:80] = header
            invalid = d.fecframe(altered, "2/3")
            result = check_result(exe, word, payload,
                                  (1 - 2 * invalid.astype(np.int16)) * 24, iterations=0)
            assert not result["valid"] and result["H"] == 0 and result["bch"]
            assert not result["header"]

        # Independent changing codewords and a fixed noise grid. Keep failures
        # visible: high-noise refusal is an outcome, never an easier replacement.
        outcomes = []
        for seed in (1021, 1022, 1023):
            current, contents = fixture(seed)
            signal = 1 - 2 * current.astype(np.int16)
            noise_rng = np.random.default_rng(seed + 9000)
            for sigma in (0.20, 0.45, 0.70, 1.00):
                channel = signal + noise_rng.normal(0, sigma, 64800)
                llr = np.clip(np.rint(channel * 16), -31, 31).astype(np.int8)
                result = check_result(exe, current, contents, llr)
                outcomes.append((seed, sigma, result["valid"], result["iterations"], result["H"]))
        assert all(row[2] for row in outcomes if row[1] == 0.20)
        assert any(not row[2] for row in outcomes if row[1] == 1.00)

        # Sparse wrong signs include layer0 collisions, degree transitions,
        # information/parity boundary and accumulator final bit.
        damaged = (signs * 24).astype(np.int8)
        positions = [0, 4, 359, 360, 4319, 4320, 43199, 43200, 64799]
        damaged[positions] = -signs[positions] * 8
        repaired = check_result(exe, word, payload, damaged)
        assert repaired["valid"] and repaired["iterations"] > 0
        print("MC6: external3 + changing GCS/partial payloads, 10980 per-tile state checks, "
              "poison/stop/resume and sparse repairs PASS")
        print("MC6 blocked candidate: 10980 additional exact per-tile state checks PASS")
        print("MC6: packed lazy-init + owned finish nibble/byte match every original decode and refusal PASS")
        print("MC6 fixed held-outs (seed,sigma,valid,iterations,H):", outcomes)


if __name__ == "__main__":
    test()
