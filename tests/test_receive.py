"""Host checks for the experimental finite S3 receiver; no radio access."""
import pathlib
import subprocess
import tempfile
import unittest
import importlib.util
import zlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReceiveTests(unittest.TestCase):
    def test_samples(self):
        source = ROOT / "firmware/rx/main"
        with tempfile.TemporaryDirectory() as tmp:
            program = pathlib.Path(tmp) / "receive"
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(source),
                            str(source / "samples.c"), str(ROOT / "tests/receive.c"),
                            "-o", str(program)], check=True)
            subprocess.run([str(program)], check=True)

    def test_download_validation(self):
        spec = importlib.util.spec_from_file_location("receive", ROOT / "host/receive.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        data = bytearray(2 * module.PAIRS)
        for n in range(module.PAIRS):
            data[2 * n] = ((13 * n) & 1023) >> 2
            data[2 * n + 1] = ((29 * n + 7) & 1023) >> 2
        report = {"cmd": "pattern", "status": "ESP_OK", "complete": True,
                  "guards_ok": True, "retained_pairs": module.PAIRS,
                  "crc32": zlib.crc32(data), "late": 0, "overruns": 0,
                  "boundary_errors": 0, "gap_events": 0, "sentinel_errors": 0}
        self.assertEqual(module.validate(data, report)["pairs"], module.PAIRS)
        for field in ("late", "overruns", "boundary_errors", "gap_events", "sentinel_errors"):
            with self.assertRaises(ValueError):
                module.validate(data, {**report, field: 1})
        with self.assertRaises(ValueError):
            module.validate(data, {**report, "dma_errors": 1})
        with self.assertRaises(ValueError):
            module.validate(data[:-2], report)
        damaged = data.copy()
        damaged[1000] ^= 1
        with self.assertRaises(ValueError):
            module.validate(damaged, report)
        # A self-consistent CRC must not make a corrupt test pattern valid.
        with self.assertRaises(ValueError):
            module.validate(damaged, {**report, "crc32": zlib.crc32(damaged)})
        snapshot = b"".join(struct.pack("<I", ((13 * n) & 1023) << 10 |
                                        ((29 * n + 7) & 1023)) for n in range(512))
        capture = {**report, "cmd": "capture", "snapshot_crc32": zlib.crc32(snapshot),
                   "acquired_pairs": 1609728, "engine_cycles": 1609728 * 15}
        self.assertEqual(module.validate(data, capture, snapshot)["raw10_control_pairs"], 512)
        with self.assertRaises(ValueError):
            module.validate(data, capture)
        with self.assertRaises(ValueError):
            module.validate(data, capture, b"\0" * 2048)
        with self.assertRaises(ValueError):
            module.validate(data, {**capture, "engine_cycles": 1609728 * 3}, snapshot)
        # A build-sized record needs an explicit matching expectation.
        short = data[:1024]
        resized = {**capture, "retained_pairs": 512, "acquired_pairs": 512,
                   "engine_cycles": 512 * 15, "crc32": zlib.crc32(short)}
        with self.assertRaises(ValueError):
            module.validate(short, resized, snapshot)
        self.assertEqual(module.validate(short, resized, snapshot, pairs=512)["pairs"], 512)
        for pairs in (0, 2000001):
            with self.assertRaises(ValueError):
                module.validate(short, resized, snapshot, pairs=pairs)

    def test_frozen_probe(self):
        spec = importlib.util.spec_from_file_location("receive", ROOT / "host/receive.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        words = [module.SENTINEL] * (3 * 16384)
        words[3:12290] = [0x123456] * (12290 - 3)
        words[16384 + 12290:16384 + 12300] = [0x789ab] * 10
        data = struct.pack("<49152I", *words)
        report = {"crc32": zlib.crc32(data), "probe": {"bytes": len(data), "before_switch": 12290}}
        result = module.inspect_banks(data, report)
        self.assertEqual(result["banks"][0]["runs_preview"], [[3, 12290]])
        self.assertEqual(result["banks"][1]["changed_words"], 10)
        self.assertEqual(result["banks"][2]["changed_words"], 0)
        with self.assertRaises(ValueError):
            module.inspect_banks(data[:-4], report)
        with self.assertRaises(ValueError):
            module.inspect_banks(data, {**report, "crc32": 0})


if __name__ == "__main__":
    unittest.main()
