#pragma once
#include <stdio.h>
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

// IDF's VFS drops characters after a 50 ms full-buffer timeout and reports
// success. Command records must survive host pauses, including their final LF.
// Picolibc calls this for stdout only; stdin and driver ownership stay with VFS.
static int console_put(char c, FILE *stream)
{
    (void)stream;
    return usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY) == 1 ? 0 : EOF;
}
