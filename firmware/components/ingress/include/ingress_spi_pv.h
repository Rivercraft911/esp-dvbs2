#pragma once
#include "ingress_spi.h"

// IDF owns the raw SPI DMA descriptors; completed transactions publish ring slots.
// Provision once (driver, queues, DMA, interrupt), then start and stop sessions without heap.
esp_err_t ingress_spi_pv_provision(void);
void ingress_spi_pv_release(void);
esp_err_t ingress_spi_pv_start(ingress_ring *ring, const ingress_spi_config *cfg,
                              volatile ingress_spi_stats *stats);
void ingress_spi_pv_rearm(ingress_desc *slot);
bool ingress_spi_pv_armed(void);
void ingress_spi_pv_stop(void);
