#ifndef INGRESS_SPI_H
#define INGRESS_SPI_H

/* ESP32-S3 SPI slave that fills an ingress ring (ingress.h) with no CPU work per byte.
 *
 * With cfg.pv, GP-SPI2 uses the IDF full-duplex slave with MISO disconnected:
 * mode 0, MSB first, raw 1332 bytes per CS, no prefix, up to 20 MHz.
 * READY additionally requires the driver's post-setup callback to have armed DMA.
 *
 * Otherwise GP-SPI2 runs as a half-duplex slave (TRM v1.8 section 30.5.9). Each message is one CS
 * assertion: command Wr_DMA (0x03 one data line, 0x13 two, 0x23 four), one address byte (0),
 * eight dummy clocks, then the 1332-byte PV message, MSB first, SPI mode 0. In single-transfer
 * mode CS rising ends the transfer and the GDMA writes end-of-frame into the slot's descriptor
 * (section 30.5.6.1, Table 30.5-7; SPI_RX_EOF_EN = 0).
 *
 * READY (active high) means the slave has room for one more message. Sample it no earlier than
 * 10 us after CS rises and before every message, as in PV-SPI v1. One instance at a time. */

#include <stdbool.h>

#include "esp_err.h"
#include "ingress.h"

typedef struct {
    int sclk, cs, d0, d1, d2, d3, ready; /* seven distinct GPIO numbers */
    bool (*received)(void *ctx); /* optional EOF ISR notification; return task-woken flag */
    void *ctx;
    uint8_t *buffers[INGRESS_SLOTS]; /* internal DMA slots, each 1336 B, owned by the caller */
    uint32_t *crc; /* 1024-word internal CRC-32 table space, owned by the caller */
    bool replay; /* diagnostic: presealed ring, DMA idle, no external sender */
    bool pv; /* raw PV-SPI v1, single-line, no command/address/dummy prefix; <=20 MHz */
} ingress_spi_config;

typedef struct {
    uint32_t eof_irqs;      /* end-of-frame interrupts taken */
    uint32_t isr_max;       /* maximum callback window, entry/exit excluded */
    uint32_t isr_cycles;    /* CPU cycles inside the interrupt callback (not entry and exit) */
} ingress_spi_stats;

/* Once per input mode, on the core that will consume (the interrupt is allocated there):
 * claim the bus and GDMA channel (quad) or the IDF slave driver (raw). Sessions then make no
 * heap calls. ingress_spi_release gives them back; it does nothing during a session. */
esp_err_t ingress_spi_provision(bool raw);
void ingress_spi_release(void);
esp_err_t ingress_spi_start(const ingress_spi_config *cfg, ingress_ring **ring);
void ingress_spi_stop(void);
void ingress_spi_get_stats(ingress_spi_stats *out);

#endif
