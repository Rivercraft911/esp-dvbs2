"""Guarded QPSK axis mapper and page expansion; also reusable in ESP32-S3 QEMU."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test():
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        (folder / "esp_attr.h").write_text("#define IRAM_ATTR\n")
        exe = folder / "axes"
        subprocess.run(["cc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-DQPSK_AXES_HOST", "-I" + tmp,
                        "-I" + str(ROOT / "firmware/main"),
                        "-I" + str(ROOT / "firmware/components/dvbs2/include"),
                        str(ROOT / "tests/qpsk_axes.c"),
                        str(ROOT / "firmware/main/machine.c"),
                        str(ROOT / "firmware/components/dvbs2/fast.c"),
                        str(ROOT / "firmware/components/dvbs2/dvbs2.c"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    test()
