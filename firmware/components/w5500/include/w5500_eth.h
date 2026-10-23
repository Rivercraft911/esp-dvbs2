#ifndef W5500_ETH_H
#define W5500_ETH_H

/* ESP-IDF side of the W5500 driver (w5500.h): a GP-SPI2 master with DMA, the S3's Ethernet MAC
 * from efuse, the settings (in NVS, or in RAM with CONFIG_W5500_SETTINGS_NVS=n) and the `eth`
 * console command. The SPI is ESP-IDF's spi_master (w5500_spi_idf.c) unless
 * CONFIG_W5500_DIRECT_SPI selects w5500_spi_direct.c, which programs GP-SPI2 and a GDMA channel
 * itself: a seventh to a quarter of the CPU per datagram (w5500.h), less internal RAM. Measured
 * in throwaway builds of the receiver before the egress (static internal RAM over the receiver
 * without W5500): +5,220 B with spi_master, +3,236 B direct, +2,756 B direct with
 * CONFIG_SPI_MASTER_ISR_IN_IRAM=n and CONFIG_SPI_SLAVE_ISR_IN_IRAM=n (either one puts GDMA control
 * code in IRAM); about 2 KB of the rest was the NVS flash-write path, which
 * CONFIG_W5500_SETTINGS_NVS=n leaves out. TX: +480 B and +544 B. An app starts the chip in one
 * role, then calls w5500_udp_send(w5500_eth(), 0, ...) or w5500_udp_recv from one task; the SPI
 * interrupt is allocated on the core that starts the port.
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
/* For an app that allocates nothing after boot (the receiver): start the SPI port now, on this
 * core, and keep it between owners, so later starts and stops take no heap; and run `eth send`
 * and `eth recv` in buffers, 2 x 1,316 bytes of DMA-capable internal RAM, 4-byte aligned, that
 * only the chip's owner touches (NULL: from the heap for the moment). A start after a bus error,
 * or with new pins or clock, starts the port again. W5500_OK, W5500_ERR_BUS or W5500_ERR_BUSY. */
int w5500_eth_reserve(uint8_t *buffers);
/* Start SPI with the settings (unless reserved), reset and configure the chip with the S3's
 * Ethernet MAC, and open socket 0 on the settings' port. The calling task owns the chip until
 * it calls w5500_eth_stop; meanwhile only it may use the chip, and other tasks (the `eth`
 * console too) get W5500_ERR_BUSY. W5500_OK or a w5500.h error; on W5500_ERR_BUS,
 * w5500_eth_error() is the ESP-IDF error behind it. */
int w5500_eth_start(w5500_eth_role role);
/* Owner only: close socket 0, free the SPI port unless reserved, and give up ownership. */
void w5500_eth_stop(void);
esp_err_t w5500_eth_error(void);
/* Payload writes whose DMA underran and were written again, since the port started (direct
 * backend; 0 with spi_master). */
uint32_t w5500_eth_underflows(void);
/* A w5500.h result as a word for JSON: "ok", "full", "spi", "chip", ... */
const char *w5500_eth_status(int e);
/* The settings: the defaults with CONFIG_W5500_DEFAULTS over them, then what `eth` changed. */
void w5500_eth_settings(w5500_settings *s);

/* `eth` console command; prints @{...} records.
 *   eth                 settings, chip version, link, SPI clock
 *   eth <setting>       w5500_settings_parse grammar, for the next start; saved in NVS, or kept
 *                       until reset with CONFIG_W5500_SETTINGS_NVS=n
 *   eth send MS [MB/S]  send numbered 7-packet TS datagrams (PID 0x100) for MS ms, paced at
 *                       MB/S or as fast as possible, and report rate and core load
 *   eth recv MS         receive for MS ms and report rate, TS sync errors and core load
 *   eth phy MODE        auto (the PMODE pins), 100f, 100h, 10f or 10h (forced, no
 *                       auto-negotiation), set after every start until reset; or reset: pulse
 *                       the PHY's reset in its mode. Reports PHYCFGR read back
 *   eth watch SECONDS   PHYCFGR every 10 ms: each link change with its time, then the totals
 * core_load is the share of the run this core was not idle, timed by an idle hook (w5500_eth.c),
 * and core_load_before the same over 20 ms just before with the console asleep. */
void w5500_eth_command(const char *args);

#endif
