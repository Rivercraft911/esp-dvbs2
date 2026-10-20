#pragma once
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "samples.h"

typedef struct {
    gdma_channel_handle_t tx, rx;
    gdma_link_list_handle_t tx_list, rx_list;
    int channel, rx_end;
    size_t bytes;
    uint8_t *out;
    uint32_t rx_flags, tx_flags;
    bool tx_connected, rx_connected, active;
} rx_spill;

esp_err_t rx_spill_open(rx_spill *spill);
esp_err_t rx_spill_begin(rx_spill *spill, const uint32_t *bank, uint16_t first,
                         size_t pairs, uint32_t *out);
esp_err_t rx_spill_finish(rx_spill *spill);
esp_err_t rx_spill_close(rx_spill *spill);
