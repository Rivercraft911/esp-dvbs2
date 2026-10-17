"""Run real radio-off fill/dump probe natively; compare complete IQ to Python, optionally save."""
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib
import numpy as np

from test_stream import NATIVE
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import qpsk_fill as q
import shaper as s
import onair


def test_probe(output=None):
    if output: output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        (folder/'native.h').write_text(NATIVE)
        for h in ('esp_err.h','esp_attr.h','esp_cpu.h','esp_heap_caps.h','esp_rom_crc.h',
                  'freertos/FreeRTOS.h','freertos/task.h','driver/usb_serial_jtag.h'):
            path = folder/h; path.parent.mkdir(exist_ok=True)
            path.write_text('#include "native.h"\n')
        # Only station_store_get is mocked; payload headers use production station_id.c.
        (folder/'station_store.h').write_text('')
        exe = folder/'probe'
        subprocess.run(['cc','-O2','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra',
                        '-Werror','-pthread',f'-I{tmp}',f'-I{ROOT}/firmware/main',
                        f'-I{ROOT}/firmware/components/dvbs2/include',
                        f'-I{ROOT}/firmware/components/radio_s3/include',
                        f'-I{ROOT}/firmware/components/station_id/include',
                        str(ROOT/'tests/qpsk_probe_host.c'),str(ROOT/'firmware/main/shaper_qpsk.c'),
                        str(ROOT/'firmware/main/shaper.c'),str(ROOT/'firmware/components/dvbs2/dvbs2.c'),
                        str(ROOT/'firmware/components/dvbs2/fast.c'),
                        str(ROOT/'firmware/components/station_id/station_id.c'),'-lm','-o',str(exe)],check=True)
        for n in (3,4):
            for mc in (4,11):
                dest = (output or folder)/f'qpsk{n}-{mc}.iq'
                out = subprocess.check_output([str(exe),str(n),str(mc),str(dest)],text=True)
                if output: (output/f'qpsk{n}-{mc}.jsonl').write_text(out)
                records = [json.loads(line[1:]) for line in out.splitlines() if line.startswith('@')]
                dump = next(r for r in records if 'crc32' in r)
                assert dump['status'] == 'ESP_OK' and dump['overlap_mismatches'] == 0
                assert dump['crc32'] == zlib.crc32(dest.read_bytes())
                phases = np.rint(np.angle(onair.symbols(mc,16,callsign='N0CALL'))/(np.pi/4)).astype(int)%8
                expected = s.iq(q.shape(q.pack(np.r_[np.ones(8,np.uint8),phases]),n))
                actual = np.fromfile(dest,dtype='<i2').reshape(-1,2)
                assert np.array_equal(actual[:,0]+1j*actual[:,1],expected)
                print(f'probe n{n} MODCOD {mc}: CRC {dump["crc32"]}, all IQ matches Python; '
                      '822 paced bank geometries and allocation failures pass')


if __name__ == '__main__':
    test_probe(pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else None)
