#pragma once
#include "ingress.h"
#include "esp_err.h"

// Preferences apply to the next encoder run. No peripheral is active between commands.
unsigned spi_input_type(void); // 0: original synthetic source; PV_TS or PV_GCS: external input
esp_err_t spi_input_start(void); // encoder core owns SPI2 and its DMA interrupt
size_t spi_input_read(uint8_t *dst, size_t n);
void spi_input_stop(void);
void spi_input_print(void); // append JSON fields after the encoder has stopped
void spi_input_command(const char *cmd, const char *args);
