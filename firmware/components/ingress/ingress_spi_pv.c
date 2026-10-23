// Raw single-line PV-SPI. The IDF full-duplex slave rearms after each CS rise.
// MISO is disconnected. READY rises only in post_setup_cb, after DMA is armed.
#include "ingress_spi_pv.h"
#include "driver/spi_slave.h"
#include "esp_private/spi_slave_internal.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static struct {
    ingress_ring *ring;
    ingress_spi_config cfg;
    volatile ingress_spi_stats *stats;
    // Keep ISR flags within the byte-load offset range as the pool grows.
    bool started, armed;
    spi_slave_transaction_t trans[INGRESS_SLOTS];
} pv;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

bool IRAM_ATTR ingress_spi_pv_armed(void) { return pv.armed; }

static void IRAM_ATTR setup(spi_slave_transaction_t *t)
{
    (void)t;
    portENTER_CRITICAL_ISR(&lock);
    pv.armed = true;
    // The logical ring credit may already be high, so refresh the physical pin.
    pv.ring->hw->ready(pv.ring->hw->ctx, pv.ring->ready == 1);
    portEXIT_CRITICAL_ISR(&lock);
}

static void IRAM_ATTR done(spi_slave_transaction_t *t)
{
    const uint32_t before = esp_cpu_get_cycle_count();
    portENTER_CRITICAL_ISR(&lock);
    pv.armed = false;
    pv.ring->hw->ready(pv.ring->hw->ctx, 0);
    ingress_desc *d = t->user;
    // Always one slot per CS, even for a long transfer. The extra word makes
    // truncation at the buffer limit distinguishable from a valid 1332 bytes.
    size_t n = (t->trans_len + 7) / 8;
    if (t->trans_len % 8 && n == PV_MSG_BYTES) n--;
    if (n > INGRESS_SLOT_BYTES) n = INGRESS_SLOT_BYTES;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->dw0 = INGRESS_SLOT_BYTES | (n << 12) | INGRESS_EOF;
    ingress_update_ready(pv.ring);
    portEXIT_CRITICAL_ISR(&lock);
    if (pv.cfg.received && pv.cfg.received(pv.cfg.ctx)) portYIELD_FROM_ISR();
    pv.stats->eof_irqs++;
    const uint32_t cycles = esp_cpu_get_cycle_count() - before;
    pv.stats->isr_cycles += cycles;
    if (cycles > pv.stats->isr_max) pv.stats->isr_max = cycles;
}

void ingress_spi_pv_rearm(ingress_desc *slot)
{
    // The consumer releases in order, at most once per completed transaction.
    // No return queue or temporary buffers: all slots are aligned internal DMA RAM.
    const unsigned i = slot - pv.ring->desc;
    ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, &pv.trans[i], 0));
    portENTER_CRITICAL(&lock);
    ingress_update_ready(pv.ring);
    portEXIT_CRITICAL(&lock);
}

esp_err_t ingress_spi_pv_start(ingress_ring *ring, const ingress_spi_config *cfg,
                              volatile ingress_spi_stats *stats)
{
    memset(&pv, 0, sizeof pv);
    pv.ring = ring; pv.cfg = *cfg; pv.stats = stats;
    // ingress_spi connects the matrix inputs itself. No slave output or
    // unused lane claims a pad.
    const spi_bus_config_t bus = {
        .mosi_io_num=-1, .miso_io_num=-1, .sclk_io_num=-1,
        .quadwp_io_num=-1, .quadhd_io_num=-1,
        .data4_io_num=-1, .data5_io_num=-1, .data6_io_num=-1, .data7_io_num=-1,
        .max_transfer_sz=INGRESS_SLOT_BYTES,
    };
    const spi_slave_interface_config_t slave = {
        .spics_io_num=-1, .mode=0, .queue_size=INGRESS_SLOTS,
        .flags=SPI_SLAVE_NO_RETURN_RESULT, .post_setup_cb=setup, .post_trans_cb=done,
    };
    esp_err_t err = spi_slave_initialize(SPI2_HOST, &bus, &slave, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    pv.started = true;
    for (unsigned i = 0; i < INGRESS_SLOTS; ++i) {
        pv.trans[i] = (spi_slave_transaction_t){.length=INGRESS_SLOT_BYTES*8,
            .rx_buffer=ring->desc[i].buf, .user=&ring->desc[i]};
        err = spi_slave_queue_trans(SPI2_HOST, &pv.trans[i], 0);
        if (err != ESP_OK) { ingress_spi_pv_stop(); return err; }
    }
    return ESP_OK;
}

void ingress_spi_pv_stop(void)
{
    if (!pv.started) return;
    // Detach CS in ingress_spi before this call. Disable the driver's interrupt
    // and discard queued work before freeing its DMA channel and transaction pool.
    spi_slave_queue_reset(SPI2_HOST);
    spi_slave_free(SPI2_HOST);
    pv.started = pv.armed = false;
}
