"""The real stdout callback must wait through USB backpressure, never drop JSON."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    p = pathlib.Path(tmp)
    (p/'driver').mkdir()
    (p/'freertos').mkdir()
    (p/'driver/usb_serial_jtag.h').write_text(
        '#include <stddef.h>\nint usb_serial_jtag_write_bytes(const void *, size_t, unsigned);\n')
    (p/'freertos/FreeRTOS.h').write_text('#define portMAX_DELAY (~0u)\n')
    (p/'test.c').write_text(r'''
#include <assert.h>
#include <string.h>
#include "console.h"
static char received[20000];
static unsigned used, stalls;
int usb_serial_jtag_write_bytes(const void *src, size_t n, unsigned timeout)
{
    assert(n == 1);
    // Model a host that pauses beyond VFS's 50 ms timeout at each full FIFO.
    // A finite write loses this byte; an indefinite write resumes after drain.
    if (used % 128 == 127) {
        if (timeout != portMAX_DELAY) return 0;
        ++stalls;
    }
    received[used++] = *(const char *)src;
    return 1;
}
int main(void)
{
    char record[16000];
    memset(record, '7', sizeof record);
    memcpy(record, "@{\"profile\":\"", 13);
    const char end[] = "\"}\n@{\"done\":true}\n";
    memcpy(record+sizeof record-sizeof end, end, sizeof end);
    for (unsigned i=0; i<sizeof record-1; ++i) assert(console_put(record[i], NULL) == 0);
    assert(stalls > 100 && used == sizeof record-1);
    assert(!memcmp(record, received, used));
    puts("console: long record and done survive more than 100 full-FIFO host pauses");
}
''')
    exe = p/'console'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',f'-I{p}',f'-I{ROOT}/firmware/main',
                    str(p/'test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
