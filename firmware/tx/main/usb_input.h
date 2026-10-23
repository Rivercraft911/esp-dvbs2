#pragma once
#include "ingress.h"
#include "esp_err.h"

esp_err_t usb_input_start(unsigned type);
void usb_input_control(bool (*control)(unsigned));
size_t usb_input_read(uint8_t *dst, size_t n);
void usb_input_stop(void);
void usb_input_print(void);
// Command task only, after the complete result and before done:true. Drain the
// binary session through its terminal NOP before allowing another console line.
// A missing NOP emits recovery diagnostics and restarts with RF already off.
void usb_input_finish(void);
