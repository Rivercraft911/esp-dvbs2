// GP-SPI2 half-duplex slave with GDMA into the ingress ring (see ingress_spi.h).
//
// The peripheral is set up with the ESP-IDF low-level layer because no IDF driver offers this
// arrangement: a free-running receive DMA over a ring of fixed slots, one CS assertion per
// slot, no CPU work per transaction. Per message the CPU only takes the end-of-frame interrupt
// (it updates READY) and later checks and releases the slot from the consumer task.

#include "ingress_spi.h"
#include "ingress_spi_pv.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "esp_private/gpio.h"
#include "esp_private/spi_common_internal.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "hal/gdma_ll.h"
#include "hal/gpio_ll.h"
#include "hal/spi_ll.h"
#include "soc/gpio_pins.h"
#include "soc/io_mux_reg.h"
#include "soc/spi_periph.h"

#define HOST SPI2_HOST
_Static_assert(sizeof(ingress_desc) == 12, "S3 GDMA descriptor layout");

static struct {
    ingress_ring ring;               // descriptors must be in internal RAM: static .bss is
    ingress_hw hw;
    ingress_spi_config cfg;
    uint32_t *crc;
    uint8_t *buffers;
    gdma_channel_handle_t rx;
    spi_dev_t *spi;
    portMUX_TYPE lock;
    volatile ingress_spi_stats stats;
    bool bus, connected, pv;
} s = {.lock = portMUX_INITIALIZER_UNLOCKED};

static IRAM_ATTR void set_ready(void *ctx, int level)
{
    (void)ctx;
    gpio_ll_set_level(&GPIO, s.cfg.ready, level && !s.cfg.replay &&
                      (!s.cfg.pv || ingress_spi_pv_armed()));
}

// Return the slave's receive path to its idle state after an overrun, as IDF's slave HD driver
// does before each receive DMA: soft reset, RX FIFO reset, clear the FIFO-full error.
static void spi_rx_reset(void)
{
    spi_ll_slave_reset(s.spi);
    spi_ll_dma_rx_fifo_reset(s.spi);
    spi_ll_infifo_full_clr(s.spi);
    spi_ll_dma_rx_enable(s.spi, 1);
}

static void rearm(void *ctx, ingress_desc *slot, bool restart)
{
    (void)ctx;
    if (s.cfg.replay) { ingress_replay_release(&s.ring, slot); return; }
    if (s.cfg.pv) { ingress_spi_pv_rearm(slot); return; }
    portENTER_CRITICAL(&s.lock);
    if (restart) {
        if (!gpio_get_level(s.cfg.cs)) { portEXIT_CRITICAL(&s.lock); return; }
        gdma_stop(s.rx);
        gdma_reset(s.rx);
        spi_rx_reset();
        s.ring.overrun = false;
        s.ring.in_long = false;
        gdma_start(s.rx, (intptr_t)slot);
    } else if (!s.ring.overrun) {
        gdma_append(s.rx);  // TRM 3.4.5: re-read the tail descriptor's next pointer
    }
    ingress_update_ready(&s.ring);
    portEXIT_CRITICAL(&s.lock);
}

static IRAM_ATTR bool on_eof(gdma_channel_handle_t chan, gdma_event_data_t *event, void *arg)
{
    const uint32_t t0 = esp_cpu_get_cycle_count();
    portENTER_CRITICAL_ISR(&s.lock);
    ingress_update_ready(&s.ring);
    portEXIT_CRITICAL_ISR(&s.lock);
    const bool woke = s.cfg.received ? s.cfg.received(s.cfg.ctx) : false;
    s.stats.eof_irqs++;
    const uint32_t cycles = esp_cpu_get_cycle_count() - t0;
    s.stats.isr_cycles += cycles;
    if (cycles > s.stats.isr_max) s.stats.isr_max = cycles;
    return woke;
}

// Data arrived with no armed slot (descriptor empty), or a descriptor failed its check. The DMA
// is restarted once the consumer has drained the ring; READY stays low until then.
static IRAM_ATTR bool on_overrun(gdma_channel_handle_t chan, gdma_event_data_t *event, void *arg)
{
    portENTER_CRITICAL_ISR(&s.lock);
    s.ring.overrun = true;
    s.ring.st.overruns++;
    ingress_update_ready(&s.ring);
    portEXIT_CRITICAL_ISR(&s.lock);
    return false;
}

static void connect_input(int pin, int signal, bool pull_up)
{
    if (pin < 0) {
        return;
    }
    gpio_func_sel(pin, PIN_FUNC_GPIO);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, pull_up ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    gpio_input_enable(pin);
    esp_rom_gpio_connect_in_signal(pin, signal, false);
}

esp_err_t ingress_spi_start(const ingress_spi_config *cfg, ingress_ring **ring)
{
    if (s.bus || s.pv || s.buffers) return ESP_ERR_INVALID_STATE;
    const int pins[] = {cfg->sclk, cfg->cs, cfg->d0, cfg->d1, cfg->d2, cfg->d3, cfg->ready};
    for (unsigned i = 0; i < 7; ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return ESP_ERR_INVALID_ARG;
        for (unsigned j = 0; j < i; ++j)
            if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    s.cfg = *cfg;
    s.stats.eof_irqs = s.stats.isr_cycles = s.stats.isr_max = 0;
    s.buffers = cfg->buffers[0] ? NULL : heap_caps_aligned_alloc(4, INGRESS_SLOTS * INGRESS_SLOT_BYTES,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!cfg->buffers[0] && !s.buffers) {
        return ESP_ERR_NO_MEM;
    }
    s.crc = heap_caps_malloc(4096,MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.crc) { ingress_spi_stop(); return ESP_ERR_NO_MEM; }
    pv_crc32_tables(s.crc);
    s.hw = (ingress_hw){.rearm = rearm, .ready = set_ready};
    ingress_init(&s.ring, s.buffers, &s.hw);
    if (cfg->buffers[0])
        for (unsigned i = 0; i < INGRESS_SLOTS; ++i) s.ring.desc[i].buf = cfg->buffers[i];
    s.ring.type = 0;

    esp_err_t err;
    if (cfg->pv) {
        // Establish READY low before the driver can arm its first transaction.
        gpio_set_level(cfg->ready, 0);
        gpio_config(&(gpio_config_t){.pin_bit_mask=1ull << cfg->ready,
                                    .mode=GPIO_MODE_INPUT_OUTPUT});
        if (!cfg->replay) {
            err = ingress_spi_pv_start(&s.ring, cfg, &s.stats);
            if (err != ESP_OK) goto fail;
            s.pv = true;
        }
        goto inputs;
    }
    err = spicommon_bus_alloc(HOST, "ingress");
    if (err != ESP_OK) {
        goto fail;
    }
    s.bus = true;
    s.spi = spi_periph_signal[HOST].hw;

    const gdma_channel_alloc_config_t dma_cfg = {
        .intr_priority = 1, .flags.isr_cache_safe = 1,
    };
    err = gdma_new_ahb_channel(&dma_cfg, NULL, &s.rx);
    if (err != ESP_OK) {
        goto fail;
    }
    err = gdma_connect(s.rx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
    if (err != ESP_OK) goto fail;
    s.connected = true;
    err = gdma_apply_strategy(s.rx, &(gdma_strategy_config_t){.owner_check = true});
    if (err != ESP_OK) goto fail;
    err = gdma_config_transfer(s.rx, &(gdma_transfer_config_t){.max_data_burst_size = 16});
    if (err != ESP_OK) goto fail;
    err = gdma_set_priority(s.rx, GDMA_LL_CHANNEL_MAX_PRIORITY);
    if (err != ESP_OK) goto fail;
    gdma_rx_event_callbacks_t cbs = {
        .on_recv_eof = on_eof,
        .on_descr_err = on_overrun,
        .on_descr_empty = on_overrun,
    };
    err = gdma_register_rx_event_callbacks(s.rx, &cbs, NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    // Half-duplex slave, mode 0, MSB first; TRM 30.5.9.4 (single transfer).
    spi_ll_slave_hd_init(s.spi);
    spi_ll_set_command_bitlen(s.spi, 8);
    spi_ll_set_addr_bitlen(s.spi, 8);
    spi_ll_set_dummy(s.spi, 8);
    spi_ll_slave_set_mode(s.spi, 0, true);
    spi_ll_set_rx_lsbfirst(s.spi, false);
    spi_ll_set_tx_lsbfirst(s.spi, false);
    spi_ll_slave_hd_set_len_cond(s.spi, SPI_LL_TRANS_LEN_COND_WRDMA);
    spi_ll_slave_set_seg_mode(s.spi, false);
    spi_ll_dma_set_rx_eof_generation(s.spi, false);  // EOF from CS rising, not a count
    spi_ll_disable_intr(s.spi, UINT32_MAX);
    spi_ll_clear_intr(s.spi, UINT32_MAX);
    spi_rx_reset();
    if (!cfg->replay) gdma_start(s.rx, (intptr_t)&s.ring.desc[0]);

inputs:
    connect_input(cfg->sclk, spi_periph_signal[HOST].spiclk_in, false);
    connect_input(cfg->cs, spi_periph_signal[HOST].spics_in, true);
    connect_input(cfg->d0, spi_periph_signal[HOST].spid_in, false);
    if (!cfg->pv) {
        connect_input(cfg->d1, spi_periph_signal[HOST].spiq_in, false);
        connect_input(cfg->d2, spi_periph_signal[HOST].spiwp_in, false);
        connect_input(cfg->d3, spi_periph_signal[HOST].spihd_in, false);
    }
    // READY starts low; input stays enabled so an on-chip sender can read the pad.
    gpio_set_level(cfg->ready, 0);
    gpio_config(&(gpio_config_t){.pin_bit_mask = 1ull << cfg->ready,
                                 .mode = GPIO_MODE_INPUT_OUTPUT});
    gpio_set_level(cfg->ready, 0);

    portENTER_CRITICAL(&s.lock);
    if (!cfg->replay) ingress_update_ready(&s.ring);
    portEXIT_CRITICAL(&s.lock);
    *ring = &s.ring;
    return ESP_OK;

fail:
    ingress_spi_stop();
    return err;
}

void ingress_spi_stop(void)
{
    pv_crc32_tables(NULL);
    heap_caps_free(s.crc); s.crc = NULL;
    if (!s.bus && !s.pv) {
        heap_caps_free(s.buffers); s.buffers = NULL;
        return;
    }
    if (s.cfg.ready >= 0) {
        gpio_set_level(s.cfg.ready, 0);
    }
    // Detach the slave inputs first: CS reads high (idle), the rest read low.
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, spi_periph_signal[HOST].spics_in,
                                   false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT,
                                   spi_periph_signal[HOST].spiclk_in, false);
    if (s.pv) { ingress_spi_pv_stop(); s.pv = false; }
    if (s.rx) {
        gdma_stop(s.rx);
        if (s.connected) gdma_disconnect(s.rx);
        s.connected = false;
        gdma_del_channel(s.rx);
        s.rx = NULL;
    }
    if (s.bus) {
        spi_ll_dma_rx_enable(s.spi, 0);
        spicommon_bus_free(HOST);
        s.bus = false;
    }
    const int pins[] = {s.cfg.sclk, s.cfg.cs, s.cfg.d0, s.cfg.d1, s.cfg.d2, s.cfg.d3};
    const size_t count = s.cfg.pv ? 3 : sizeof pins / sizeof pins[0];
    for (size_t i = 0; i < count; ++i) {
        if (pins[i] >= 0) {
            gpio_reset_pin(pins[i]);
        }
    }
    // Keep READY actively low between sessions. A floating pad could advertise credit.
    gpio_set_level(s.cfg.ready, 0);
    gpio_set_direction(s.cfg.ready, GPIO_MODE_INPUT_OUTPUT);
    heap_caps_free(s.buffers);
    s.buffers = NULL;
}

void ingress_spi_get_stats(ingress_spi_stats *out)
{
    out->eof_irqs = s.stats.eof_irqs;
    out->isr_cycles = s.stats.isr_cycles;
    out->isr_max = s.stats.isr_max;
}
