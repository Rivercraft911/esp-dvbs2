"""Deterministic bank provenance, independent stream oracle and sanitizers."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import rx_filter


def main():
    bank = rx_filter.design()
    assert rx_filter.BANK.read_text() == rx_filter.header(bank), "stale coefficient bank"
    p, d, h, f, response, scale = bank
    assert p.dtype == np.int8 and d.dtype == np.int16
    assert p.shape == d.shape == (64, 32)
    assert max(abs(p.astype(int)).sum(axis=1))*128 < 2**31
    assert max(abs(d.astype(int)).sum(axis=1))*128 < 2**31
    # Independent central finite difference of the continuous equation,
    # including the minus sign from moving the sample center.
    t = np.arange(32)[None, :] - 15.5 - np.arange(64)[:, None]/64
    epsilon = 1e-4
    finite = (rx_filter.sampled_pulse(t-epsilon, f, response) -
              rx_filter.sampled_pulse(t+epsilon, f, response))/(2*epsilon*scale)
    assert np.max(abs(finite-d.astype(float)/32768)) < 1.6e-5
    report = rx_filter.report(bank)
    # Quality is reported separately, never turned into a packet-tuned gate.
    assert report["experimental"] and not report["rf_qualified"]
    assert all(np.isfinite(x) for x in report["metrics"].values())
    print(json.dumps(report, sort_keys=True))
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp)/"rx-filter"
        command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I"+str(ROOT/"firmware/rx/dsp"),
                        str(ROOT/"firmware/rx/dsp/rx_filter.c"),
                        str(ROOT/"tests/rx_filter.c"), "-o", str(exe)]
        for batch in (0, 1):
            for mock in ([], ["-D__XTENSA__", "-DRX_FILTER_MOCK_PIE"]):
                subprocess.run(command + [f"-DRX_FILTER_BATCH={batch}"] + mock, check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
