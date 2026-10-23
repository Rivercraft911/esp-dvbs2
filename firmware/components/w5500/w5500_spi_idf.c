/* ESP-IDF spi_master backend for the W5500 driver (w5500_port.h); the reference for the direct
 * GP-SPI2 backend. */
#include "w5500_port.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

#define HOST SPI2_HOST

w5500_port_info w5500_port = {.name = "esp-idf"};

static struct {
    spi_device_handle_t dev;
    bool bus, acquired;
    spi_transaction_t t;
    uint8_t small[W5500_SMALL_MAX] __attribute__((aligned(4)));
} idf;

/* One W5500 frame (DS 2.2) as one ESP-IDF half-duplex transaction: the 16-bit offset is the
 * command phase, the control byte the address phase, then the data phase. Short frames are polled
 * through an internal buffer, so callers' stacks may sit in any memory; payload bursts run as
 * interrupt DMA transactions on the caller's buffer, never reallocated (ALIGN_MANUAL). Nothing is
 * posted: every frame has finished when this returns. */
static int frame(void *ctx, uint16_t offset, uint8_t control, const uint8_t *tx, uint8_t *rx,
                 size_t n, bool post)
{
    (void)ctx;
    (void)post;
    spi_transaction_t *t = &idf.t;
    const bool small = n <= W5500_SMALL_MAX;
    *t = (spi_transaction_t){
        .flags = SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL, .cmd = offset, .addr = control};
    if (tx) {
        t->length = 8 * n;
        t->tx_buffer = small ? memcpy(idf.small, tx, n) : tx;
    } else {
        t->rxlength = 8 * n;
        t->rx_buffer = small ? idf.small : rx;
    }
    esp_err_t err;
    if (small) {
        err = spi_device_polling_transmit(idf.dev, t);
        w5500_port.polled++;
    } else {
        err = spi_device_transmit(idf.dev, t);   /* the task sleeps while DMA moves the payload */
        w5500_port.queued++;
    }
    if (err != ESP_OK) {
        w5500_port.err = err;
        return 1;
    }
    if (small && rx) memcpy(rx, idf.small, n);
    return 0;
}

static void delay(void *ctx, uint32_t us)
{
    (void)ctx;
    esp_rom_delay_us(us);   /* reset and command waits only */
}

void w5500_port_stop(void)
{
    if (idf.acquired) spi_device_release_bus(idf.dev);
    if (idf.dev) spi_bus_remove_device(idf.dev);
    if (idf.bus) spi_bus_free(HOST);
    idf.dev = NULL;
    idf.bus = idf.acquired = false;
}

esp_err_t w5500_port_start(const w5500_settings *s, w5500_bus *bus)
{
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    w5500_port = (w5500_port_info){.name = "esp-idf"};
    const spi_bus_config_t cfg = {
        .mosi_io_num = s->mosi, .miso_io_num = s->miso, .sclk_io_num = s->sclk,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = W5500_UDP_MAX,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,   /* the calling core */
    };
    const spi_device_interface_config_t dev = {
        .command_bits = 16, .address_bits = 8,      /* offset and control byte (DS 2.2) */
        .mode = 0,                                  /* DS p.12 */
        .clock_speed_hz = s->sck_khz * 1000,
        .input_delay_ns = 5,                        /* MISO valid 5 ns after SCLK falls (DS p.61) */
        .spics_io_num = s->cs,
        .cs_ena_pretrans = 1, .cs_ena_posttrans = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_initialize(HOST, &cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        idf.bus = true;
        err = spi_bus_add_device(HOST, &dev, &idf.dev);
    }
    if (err == ESP_OK) err = spi_device_acquire_bus(idf.dev, portMAX_DELAY);
    if (err != ESP_OK) {
        w5500_port.err = err;
        w5500_port_stop();
        return err;
    }
    idf.acquired = true;
    spi_device_get_actual_freq(idf.dev, &w5500_port.khz);
    w5500_port.heap_bytes = free_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *bus = (w5500_bus){.frame = frame, .delay_us = delay};
    return ESP_OK;
}
