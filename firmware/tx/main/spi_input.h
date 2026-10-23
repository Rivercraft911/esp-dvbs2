#pragma once
#include "ingress.h"
#include "esp_err.h"

// Preferences apply to the next encoder run. No peripheral is active between commands.
const char *spi_input_source(void); // "spi" or "usb" for external input
unsigned spi_input_type(void); // 0: original synthetic source; PV_TS or PV_GCS: external input
void spi_input_control(bool (*control)(unsigned)); // configure before start; called on encoder core
void spi_input_buffers(uint8_t *tail, size_t tail_bytes, uint8_t *workspace, size_t workspace_bytes);
esp_err_t spi_input_start(void); // on the encoder core; spiinput provisioned SPI2 on core 0
size_t spi_input_read(uint8_t *dst, size_t n);
void spi_input_stop(void);
void spi_input_print(void); // append JSON fields after the encoder has stopped
// The running session's ring, or NULL: for the status line, read on the encoder's core with
// its interrupts masked.
const ingress_ring *spi_input_ring(void);
void spi_input_command(const char *cmd, const char *args);
