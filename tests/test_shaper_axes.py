"""Lossless five-level table and sample cuts, with an independent full-table oracle."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        folder=Path(tmp)
        (folder/'esp_attr.h').write_text('#define IRAM_ATTR\n')
        exe=folder/'axes'
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+tmp,'-I'+str(ROOT/'firmware/components/shaper/include'),
                        str(ROOT/'tests/shaper_axes.c'),str(ROOT/'firmware/components/shaper/shaper.c'),
                        str(ROOT/'firmware/components/shaper/shaper_axes.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
