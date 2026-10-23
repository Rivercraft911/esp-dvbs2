#ifndef W5500_GPSPI_H
#define W5500_GPSPI_H

/* Register encodings for the direct ESP32-S3 GP-SPI2 + GDMA backend (w5500_spi_direct.c). Portable
 * C99, so tests/test_w5500.py checks them against its own model of how the controller shifts bits
 * out. "TRM" is the ESP32-S3 Technical Reference Manual, Version 1.8. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define W5500_GPSPI_CPU_MAX 64      /* data bytes in SPI_W0_REG..SPI_W15_REG (TRM 30.5.5) */
#define W5500_GDMA_MAX 4095         /* bytes per GDMA descriptor (TRM 3.4.1, size/length 12 bits) */

/* SPI_ADDR_REG for the 3-byte W5500 header sent as one 24-bit address phase
 * (SPI_USR_ADDR_BITLEN = 23). With SPI_WR_BIT_ORDER = 0 (MSB first, Register 30.6, p.1158) the
 * controller sends SPI_USR_ADDR_VALUE[31:8] from bit 31 down (Table 30.5-10, p.1127): offset
 * MSB, offset LSB, control byte, exactly the W5500 frame head (W5500 DS 2.2, p.14). A 16-bit
 * command phase would go out low byte first (Table 30.5-9), so it is not used. */
uint32_t w5500_gpspi_addr(uint16_t offset, uint8_t control);

/* SPI_CLOCK_REG for SPI_CLK = src / n (n = 2..64, SPI_CLKDIV_PRE = 0), the fastest rate not
 * above want (TRM 30.7, p.1142; Register 30.13, p.1164): SPI_CLKCNT_N = n - 1 [17:12],
 * SPI_CLKCNT_H = floor(n / 2) - 1 [11:6], SPI_CLKCNT_L = SPI_CLKCNT_N [5:0]. Returns 0 if
 * want < src / 64; *actual_hz gets the rate. */
uint32_t w5500_gpspi_clock(uint32_t src_hz, uint32_t want_hz, uint32_t *actual_hz);

/* GDMA descriptor word 0 (TRM 3.4.1, p.360): owner [31] = 1 (the GDMA may use the buffer),
 * suc_eof [30], length [23:12], size [11:0]. A transmit descriptor carries length = size and
 * eof; a receive descriptor carries size, length 0 and eof 0 (the GDMA fills them in). */
uint32_t w5500_gdma_dw0(unsigned size, unsigned length, bool eof);

/* CPU-controlled data: byte i is bits 8(i % 4) + 7 .. 8(i % 4) of SPI_W(i / 4)_REG and bytes go
 * out from byte 0 (TRM 30.5.5.1 note, p.1116; 30.5.8.2, p.1126), each MSB first. */
void w5500_gpspi_load(volatile uint32_t *w, const uint8_t *p, size_t n);
void w5500_gpspi_store(uint8_t *p, const volatile uint32_t *w, size_t n);

#endif
