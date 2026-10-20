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


if __name__ == "__main__":
    unittest.main()
