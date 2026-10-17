"""Raw-phase shaper glue against the legacy C oracle, all edges, tails and bank wraps.

Optional QEMU log: require matching hashes from real PIE instructions. Timing is not measured.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    directory = pathlib.Path(tmp)
    (directory / 'esp_attr.h').write_text('#define IRAM_ATTR\n')
    exe = directory / 'samples'
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DTX_HOST',
                    '-I' + tmp, '-I' + str(ROOT / 'firmware/main'),
                    str(ROOT / 'tests/tx_samples.c'), str(ROOT / 'firmware/main/shaper.c'),
                    '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    records = [json.loads(line[1:]) for line in out.splitlines()]
    assert all(r['mismatch'] == 0 and r['guards'] == 1 for r in records), records
    print(out, end='')
    if len(sys.argv) > 1:
        qemu = [json.loads(line[1:]) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
                if line.startswith('@{"test":"tx_samples"')]
        assert qemu == records, (qemu, records)
        print('QEMU PIE raw-phase kernels and sample glue equal the host oracle in every case')
