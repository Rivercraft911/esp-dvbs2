#ifndef W5500_ETH_H
#define W5500_ETH_H

/* ESP-IDF side of the W5500 driver (w5500.h): a GP-SPI2 master with DMA, the S3's Ethernet MAC
 * from efuse, settings in NVS and the `eth` console command. The SPI is ESP-IDF's spi_master
 * (w5500_spi_idf.c) unless CONFIG_W5500_DIRECT_SPI selects w5500_spi_direct.c, which programs
 * GP-SPI2 and a GDMA channel itself: a seventh to a quarter of the CPU per datagram (w5500.h),
 * less internal RAM. Measured in throwaway builds of the receiver (static internal RAM over
 * today's receiver): +5,220 B with spi_master, +3,236 B direct, +2,756 B direct with
 * CONFIG_SPI_MASTER_ISR_IN_IRAM=n and CONFIG_SPI_SLAVE_ISR_IN_IRAM=n (either one puts GDMA control
 * code in IRAM); about 2 KB of the rest is the NVS flash-write path, and adding
 * CONFIG_SPI_FLASH_ROM_IMPL=y brings the receiver 6,004 B below today. TX: +480 B and +544 B. An
 * app starts the chip in one role, then calls w5500_udp_send(w5500_eth(), 0, ...) or
 * w5500_udp_recv from one task; the SPI interrupt is allocated on that task's core.
 *
 * Wiring, the same on both XIAO ESP32-S3 boards (the TX's SPI input preset uses GPIO 1, 2 and
 * 4-8; the receiver uses no GPIOs). 3.3 V logic on both sides; keep the wires short.
 *
 * | W5500 module | XIAO ESP32-S3 | Notes                                                        |
 * |--------------|---------------|--------------------------------------------------------------|
 * | SCLK         | D10 (GPIO9)   |                                                              |
 * | MOSI         | D6 (GPIO43)   | U0TXD: ROM boot text toggles it while CS is high, ignored    |
 * | MISO         | D7 (GPIO44)   | U0RXD; the W5500 drives it only while CS is low (DS p.10)    |
 * | SCS          | D2 (GPIO3)    | strap pin, read only with JTAG efuses set; the W5500's       |
 * |              |               | 50-112 kOhm pull-up holds CS high through boot (DS p.9, 59)  |
 * | 3.3V         | 3V3           | the chip draws 128-132 mA on a 100 Mb/s link (DS p.59-60)    |
 * | GND          | GND           |                                                              |
 * | RST          | not connected | internal pull-up (DS p.10); power-on reset, then MR.RST      |
 * | INT          | not connected | polled: an empty receive poll costs 12.5 us (w5500.h)        |
 *
 * Other boards: `eth pins SCLK MOSI MISO CS`, any four free GPIOs through the GPIO matrix.
 * The W5500 has no auto-MDIX (DS p.62); a laptop port without it needs a crossover cable.
 * GP-SPI2 is also the TX's SPI input slave (ingress_spi.c): one or the other per run. */

#include "esp_err.h"
#include "w5500.h"

typedef enum {
    W5500_ETH_SEND,   /* socket 0 gets the 16 KB TX memory and the stored destination */
    W5500_ETH_RECV,   /* socket 0 gets the 16 KB RX memory */
} w5500_eth_role;

/* The one chip on the bus. */
w5500 *w5500_eth(void);
/* Start SPI with the stored settings, reset and configure the chip with the S3's Ethernet MAC,
 * and open socket 0 on the stored port. The calling task owns the chip until it calls
 * w5500_eth_stop; meanwhile only it may use the chip, and other tasks (the `eth` console too)
 * get W5500_ERR_BUSY. W5500_OK or a w5500.h error; on W5500_ERR_BUS, w5500_eth_error() is the
 * ESP-IDF error behind it. */
int w5500_eth_start(w5500_eth_role role);
/* Owner only: close socket 0, free the SPI bus and give up ownership. */
void w5500_eth_stop(void);
esp_err_t w5500_eth_error(void);
/* The stored settings, or the defaults. */
void w5500_eth_settings(w5500_settings *s);

/* `eth` console command; prints @{...} records.
 *   eth                 settings, chip version, link, SPI clock
 *   eth <setting>       w5500_settings_parse grammar, saved in NVS for the next start
 *   eth send MS [MB/S]  send numbered 7-packet TS datagrams (PID 0x100) for MS ms, paced at
 *                       MB/S or as fast as possible, and report rate and core load
 *   eth recv MS         receive for MS ms and report rate, TS sync errors and core load */
void w5500_eth_command(const char *args);

#endif
