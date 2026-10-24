#ifndef W5500_PORT_H
#define W5500_PORT_H

/* The SPI under w5500_eth.c: ESP-IDF's spi_master (w5500_spi_idf.c, the reference) or the
 * GP-SPI2 registers and GDMA driven directly (w5500_spi_direct.c), chosen by
 * CONFIG_W5500_DIRECT_SPI. */

#include "esp_err.h"
#include "w5500.h"

typedef struct {
    const char *name;           /* "esp-idf" or "direct" */
    esp_err_t err;              /* the ESP-IDF error behind a failed start or frame */
    uint32_t polled, queued;    /* frames run by the CPU, frames run by DMA */
    uint32_t sleeps;            /* waits that slept on the SPI interrupt (direct backend) */
    uint32_t underflows;        /* payload writes the GDMA underran, written again (direct) */
    const char *why;            /* the wait that failed a frame (direct), or NULL */
    uint32_t recoveries;        /* GP-SPI2 and GDMA reset after REWRITES underflows in a row */
    uint32_t prefill_waits;     /* writes whose first data took over PREFILL_US to arrive */
    int khz;                    /* SPI clock */
    size_t heap_bytes;          /* internal RAM taken at start */
} w5500_port_info;

extern w5500_port_info w5500_port;

/* Claim GP-SPI2 and the pins and fill *bus. Call from the task, and core, that uses the chip. */
esp_err_t w5500_port_start(const w5500_settings *s, w5500_bus *bus);
void w5500_port_stop(void);

#endif
