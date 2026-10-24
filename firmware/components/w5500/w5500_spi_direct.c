/* Direct GP-SPI2 master + GDMA backend for the W5500 driver (CONFIG_W5500_DIRECT_SPI,
 * w5500_port.h).
 *
 * Written from the ESP32-S3 Technical Reference Manual, Version 1.8 ("TRM"): chapters 3 (GDMA),
 * 6 (IO MUX and GPIO matrix), 17 (system registers) and 30 (SPI). Each address and bit below is
 * named as in the TRM and checked at compile time against ESP-IDF's generated soc/ headers.
 * ESP-IDF is used only where something is shared with other drivers: the peripheral clock lock
 * (PERIPH_RCC_ATOMIC), GDMA channel ownership (gdma_new_ahb_channel, gdma_connect) and the SPI2
 * interrupt line (esp_intr_alloc). The data path is register writes only: no heap, no IRAM, no
 * ESP-IDF calls.
 *
 * Per W5500 frame (w5500_bus.frame), half-duplex master, mode 0, MSB first (TRM 30.5.8.4):
 *   always   SPI_ADDR_REG = offset << 16 | control << 8: the 3-byte W5500 header is the 24-bit
 *            address phase, so no data is copied to prepend it (w5500_gpspi.h);
 *            SPI_MS_DLEN_REG = 8n - 1 (Register 30.7).
 *   n <= 64  CPU-controlled (TRM 30.5.5): SPI_DMA_CONF_REG clears the DMA enables and resets
 *            buf_tx_afifo and spi_rx_afifo; data into SPI_W0..W15 for a write; SPI_USER_REG =
 *            SPI_USR_ADDR | SPI_USR_MOSI (write) or SPI_USR_MISO (read); start; spin on SPI_USR;
 *            data out of SPI_W0..W15 for a read.
 *   n > 64   DMA-controlled (TRM 30.5.6, 3.6.2/3.6.3): one GDMA descriptor on the caller's buffer;
 *            SPI_DMA_CONF_REG resets the FIFOs and sets SPI_DMA_TX_ENA or SPI_DMA_RX_ENA; the GDMA
 *            channel is reset (GDMA_OUT_RST / GDMA_IN_RST, 1 then 0) and started at the descriptor
 *            (GDMA_OUTLINK_START / GDMA_INLINK_START); SPI_USER_REG as above; start. A posted
 *            write returns here; a read waits until SPI_USR clears and GDMA_IN_SUC_EOF says the
 *            data is in memory (TRM Table 30.10-2 note 1).
 *   start    clear SPI_TRANS_DONE_INT, SPI_UPDATE (sync APB to SPI clock domain, Register 30.1)
 *            and wait for it to clear, then SPI_USR.
 * The SPI FIFOs are reset before the GDMA starts, not after as TRM 30.5.8.4 step 9 lists, so no
 * byte the GDMA has already pushed is dropped (ESP-IDF's driver uses the same order).
 * Waiting: a transfer with more than SLEEP_US left sleeps on SPI_TRANS_DONE_INT (a flash-resident
 * interrupt that gives a semaphore), anything shorter spins on SPI_USR. Every wait has a deadline;
 * one that passes aborts: both GDMA links are stopped and reset, GP-SPI2 goes through its
 * peripheral reset, and later frames fail until the chip is started again, so no buffer stays
 * with the GDMA after an error is returned.
 * Ownership: GP-SPI2 is claimed through ESP-IDF's SPI-common allocator (spicommon_bus_alloc, as
 * the TX's SPI input does), so neither can take it from the other; the GDMA channels come from
 * ESP-IDF's allocator and are set to a known configuration, whatever a previous owner left. */
#include "w5500_port.h"
#include "w5500_gpspi.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_private/gdma.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/spi_common_internal.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/gdma_reg.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "soc/io_mux_reg.h"
#include "soc/reg_base.h"
#include "soc/spi_reg.h"
#include "soc/system_reg.h"

/* Bases, TRM Table 4.3-3 (pp.408-409). */
#define SPI2_BASE 0x60024000u        /* SPI Controller 2 (GP-SPI2) */
#define GDMA_BASE 0x6003F000u
#define SYSTEM_BASE 0x600C0000u
#define GPIO_BASE 0x60004000u
#define IOMUX_BASE 0x60009000u

/* GP-SPI2 registers, TRM 30.11 (p.1151) and 30.12. */
#define G_CMD (SPI2_BASE + 0x00)          /* SPI_CMD_REG: SPI_USR [24], SPI_UPDATE [23] */
#define G_ADDR (SPI2_BASE + 0x04)         /* SPI_ADDR_REG: SPI_USR_ADDR_VALUE */
#define G_CTRL (SPI2_BASE + 0x08)         /* SPI_CTRL_REG: line modes, bit orders */
#define G_CLOCK (SPI2_BASE + 0x0C)        /* SPI_CLOCK_REG */
#define G_USER (SPI2_BASE + 0x10)         /* SPI_USER_REG */
#define G_USER1 (SPI2_BASE + 0x14)        /* SPI_USER1_REG: SPI_USR_ADDR_BITLEN [31:27] */
#define G_USER2 (SPI2_BASE + 0x18)        /* SPI_USER2_REG: command phase */
#define G_MS_DLEN (SPI2_BASE + 0x1C)      /* SPI_MS_DLEN_REG: SPI_MS_DATA_BITLEN [17:0] */
#define G_MISC (SPI2_BASE + 0x20)         /* SPI_MISC_REG: CS lines, clock idle level */
#define G_DIN_MODE (SPI2_BASE + 0x24)     /* SPI_DIN_MODE_REG: input timing, 0 = none */
#define G_DIN_NUM (SPI2_BASE + 0x28)      /* SPI_DIN_NUM_REG */
#define G_DOUT_MODE (SPI2_BASE + 0x2C)    /* SPI_DOUT_MODE_REG */
#define G_DMA_CONF (SPI2_BASE + 0x30)     /* SPI_DMA_CONF_REG */
#define G_INT_ENA (SPI2_BASE + 0x34)      /* SPI_DMA_INT_ENA_REG */
#define G_INT_CLR (SPI2_BASE + 0x38)      /* SPI_DMA_INT_CLR_REG */
#define G_INT_RAW (SPI2_BASE + 0x3C)      /* SPI_DMA_INT_RAW_REG */
#define G_INT_ST (SPI2_BASE + 0x40)       /* SPI_DMA_INT_ST_REG */
#define G_W0 (SPI2_BASE + 0x98)           /* SPI_W0_REG .. SPI_W15_REG at 0xD4 */
#define G_SLAVE (SPI2_BASE + 0xE0)        /* SPI_SLAVE_REG: SPI_SLAVE_MODE [26] */
#define G_CLK_GATE (SPI2_BASE + 0xE8)     /* SPI_CLK_GATE_REG (p.1165) */

#define CMD_USR (1u << 24)
#define CMD_UPDATE (1u << 23)
#define USER_ADDR (1u << 30)              /* SPI_USR_ADDR */
#define USER_MISO (1u << 28)              /* SPI_USR_MISO */
#define USER_MOSI (1u << 27)              /* SPI_USR_MOSI */
#define USER1_ADDR_24 (23u << 27)         /* SPI_USR_ADDR_BITLEN = 24 - 1 */
#define CTRL_MODES 0x0781C7E8u            /* bit orders, FREAD/FCMD/FADDR 2/4/8-bit, DUMMY_OUT */
#define MISC_CS_DIS 0x3Eu                 /* SPI_CS1_DIS .. SPI_CS5_DIS; CS0 is the W5500's */
#define MISC_CLEAR 0x60001FFFu            /* CS_KEEP_ACTIVE, CK_IDLE_EDGE, MASTER_CS_POL, CK_DIS,
                                           * CS0..CS5_DIS */
#define DMA_AFIFO_RST (1u << 31)          /* SPI_DMA_AFIFO_RST */
#define BUF_AFIFO_RST (1u << 30)          /* SPI_BUF_AFIFO_RST */
#define RX_AFIFO_RST (1u << 29)           /* SPI_RX_AFIFO_RST */
#define DMA_TX_ENA (1u << 28)
#define DMA_RX_ENA (1u << 27)
#define FIFO_RST (DMA_AFIFO_RST | BUF_AFIFO_RST | RX_AFIFO_RST)
#define TRANS_DONE (1u << 12)             /* SPI_TRANS_DONE_INT_{ENA,CLR,RAW,ST} */
#define OUTFIFO_EMPTY (1u << 1)           /* SPI_DMA_OUTFIFO_EMPTY_ERR_INT_{CLR,RAW}: the GDMA
                                           * gave less data than the SPI shifted out */
#define REWRITES 10                       /* tries after an underflow before a frame fails */
#define CLK_GATE_ON 0x7u                  /* CLK_EN, MST_CLK_ACTIVE, MST_CLK_SEL = PLL_F80M */
#define SPI_SRC_HZ 80000000u              /* PLL_F80M_CLK (Register 30.14, p.1165) */

/* GDMA channel n registers, TRM 3.7 (p.369): base + 0xC0 n. */
#define CH_STRIDE 0xC0u
#define IN_CONF0 0x00u                    /* GDMA_IN_CONF0_CHn: GDMA_IN_RST_CHn [0] */
#define IN_CONF1 0x04u                    /* GDMA_IN_CONF1_CHn: GDMA_IN_CHECK_OWNER_CHn [12] */
#define IN_INT_ENA 0x10u
#define IN_INT_RAW 0x08u                  /* GDMA_IN_SUC_EOF_CHn [1], GDMA_IN_DSCR_ERR_CHn [3] */
#define IN_INT_CLR 0x14u
#define IN_LINK 0x20u                     /* GDMA_INLINK_ADDR_CHn [19:0], _START_CHn [22] */
#define OUT_CONF0 0x60u                   /* GDMA_OUT_RST_CHn [0] */
#define OUT_CONF1 0x64u                   /* GDMA_OUT_CHECK_OWNER_CHn [12] */
#define OUT_INT_ENA 0x70u
#define OUT_INT_CLR 0x74u
#define OUT_LINK 0x80u                    /* GDMA_OUTLINK_ADDR_CHn [19:0], _START_CHn [21] */
#define IN_SUC_EOF (1u << 1)
#define IN_DSCR_ERR (1u << 3)
#define INLINK_START (1u << 22)
#define INLINK_STOP (1u << 21)
#define OUTLINK_START (1u << 21)
#define OUTLINK_STOP (1u << 20)
#define CHECK_OWNER (1u << 12)
/* GDMA_IN_CONF0_CHn: IN_LOOP_TEST, INDSCR_BURST_EN, IN_DATA_BURST_EN, MEM_TRANS_EN (TRM 3.8) */
#define IN_CONF0_OFF 0x1Eu
/* GDMA_OUT_CONF0_CHn: OUT_LOOP_TEST, OUT_AUTO_WRBACK, OUTDSCR_BURST_EN, OUT_DATA_BURST_EN */
#define OUT_CONF0_OFF 0x36u
#define OUT_EOF_MODE (1u << 3)            /* EOF once the last byte has left the GDMA */
#define OUT_BURST (3u << 4)               /* GDMA_OUTDSCR_BURST_EN, GDMA_OUT_DATA_BURST_EN */
#define OUT_FIFO 0x78u                    /* GDMA_OUTFIFO_STATUS_CHn */
#define OUT_FIFO_EMPTY 0x2Au              /* GDMA_OUTFIFO_EMPTY_L1, _L2, _L3_CHn */
#define PREFILL_US 10                     /* the longest a write waits for its first data */
#define LINK_ADDR 0xFFFFFu
#define DMA_RAM_LOW 0x3FC88000u           /* internal RAM the GDMA reaches (TRM 3.4.8, p.364) */
#define DMA_RAM_HIGH 0x3FD00000u

/* System registers, TRM 17.3.5 (p.825): SPI2's clock gate and reset. */
#define PERIP_CLK_EN0 (SYSTEM_BASE + 0x18)
#define PERIP_RST_EN0 (SYSTEM_BASE + 0x20)
#define SYS_SPI2 (1u << 6)                /* SYSTEM_SPI2_CLK_EN, SYSTEM_SPI2_RST */

/* GPIO matrix and IO MUX, TRM 6.14-6.15. */
#define GM_ENABLE_W1TS (GPIO_BASE + 0x24)
#define GM_ENABLE_W1TC (GPIO_BASE + 0x28)
#define GM_ENABLE1_W1TS (GPIO_BASE + 0x30)
#define GM_ENABLE1_W1TC (GPIO_BASE + 0x34)
#define GM_IN_SEL(y) (GPIO_BASE + 0x154 + 4u * (y))   /* GPIO_FUNCy_IN_SEL_CFG_REG */
#define GM_OUT_SEL(x) (GPIO_BASE + 0x554 + 4u * (x))  /* GPIO_FUNCx_OUT_SEL_CFG_REG */
#define IN_SEL_MATRIX (1u << 7)                         /* GPIO_SIGy_IN_SEL */
#define OUT_OEN_SEL (1u << 10)                          /* GPIO_FUNCx_OEN_SEL: OE = GPIO_ENABLE */
#define OUT_SIMPLE 0x100u                               /* signal 256: plain GPIO output */
#define IOMUX(n) (IOMUX_BASE + 0x04 + 4u * (n))         /* IO_MUX_GPIOn_REG */
#define IOMUX_WPU (1u << 8)
#define IOMUX_IE (1u << 9)
#define IOMUX_DRV_2 (2u << 10)                          /* IO_MUX_FUN_DRV, default strength */
#define IOMUX_GPIO (1u << 12)                           /* IO_MUX_MCU_SEL = 1: GPIO, Table 6.12-1 */
#define SIG_FSPICLK 101u                                /* Table 6.11-1, p.486 */
#define SIG_FSPIQ 102u
#define SIG_FSPID 103u
#define SIG_FSPICS0 110u                                /* p.487 */

/* The constants above, read from the TRM, against ESP-IDF's generated register headers. */
_Static_assert(SPI_CMD_REG(2) == G_CMD && SPI_ADDR_REG(2) == G_ADDR && SPI_CTRL_REG(2) == G_CTRL &&
               SPI_CLOCK_REG(2) == G_CLOCK && SPI_USER_REG(2) == G_USER &&
               SPI_USER1_REG(2) == G_USER1 && SPI_USER2_REG(2) == G_USER2 &&
               SPI_MS_DLEN_REG(2) == G_MS_DLEN && SPI_MISC_REG(2) == G_MISC &&
               SPI_DIN_MODE_REG(2) == G_DIN_MODE && SPI_DIN_NUM_REG(2) == G_DIN_NUM &&
               SPI_DOUT_MODE_REG(2) == G_DOUT_MODE && SPI_DMA_CONF_REG(2) == G_DMA_CONF &&
               SPI_DMA_INT_ENA_REG(2) == G_INT_ENA && SPI_DMA_INT_CLR_REG(2) == G_INT_CLR &&
               SPI_DMA_INT_RAW_REG(2) == G_INT_RAW &&
               SPI_DMA_INT_ST_REG(2) == G_INT_ST && SPI_W0_REG(2) == G_W0 &&
               SPI_SLAVE_REG(2) == G_SLAVE && SPI_CLK_GATE_REG(2) == G_CLK_GATE, "SPI2 registers");
_Static_assert(SPI_USR == CMD_USR && SPI_UPDATE == CMD_UPDATE && SPI_USR_ADDR == USER_ADDR &&
               SPI_USR_MISO == USER_MISO && SPI_USR_MOSI == USER_MOSI &&
               SPI_USR_ADDR_BITLEN_S == 27 && SPI_DMA_AFIFO_RST == DMA_AFIFO_RST &&
               SPI_BUF_AFIFO_RST == BUF_AFIFO_RST && SPI_RX_AFIFO_RST == RX_AFIFO_RST &&
               SPI_DMA_TX_ENA == DMA_TX_ENA && SPI_DMA_RX_ENA == DMA_RX_ENA &&
               SPI_TRANS_DONE_INT_ENA == TRANS_DONE &&
               SPI_DMA_OUTFIFO_EMPTY_ERR_INT_RAW == OUTFIFO_EMPTY &&
               SPI_DMA_OUTFIFO_EMPTY_ERR_INT_CLR == OUTFIFO_EMPTY &&
               (SPI_CLK_EN | SPI_MST_CLK_ACTIVE | SPI_MST_CLK_SEL) == CLK_GATE_ON, "SPI2 bits");
_Static_assert(CTRL_MODES == (SPI_WR_BIT_ORDER << SPI_WR_BIT_ORDER_S | SPI_RD_BIT_ORDER <<
               SPI_RD_BIT_ORDER_S | SPI_FREAD_OCT | SPI_FREAD_QUAD | SPI_FREAD_DUAL | SPI_FCMD_OCT |
               SPI_FCMD_QUAD | SPI_FCMD_DUAL | SPI_FADDR_OCT | SPI_FADDR_QUAD | SPI_FADDR_DUAL |
               SPI_DUMMY_OUT), "SPI_CTRL_REG mode bits");
_Static_assert(MISC_CLEAR == (SPI_CS_KEEP_ACTIVE | SPI_CK_IDLE_EDGE |
               SPI_MASTER_CS_POL << SPI_MASTER_CS_POL_S | SPI_CK_DIS | SPI_CS5_DIS | SPI_CS4_DIS |
               SPI_CS3_DIS | SPI_CS2_DIS | SPI_CS1_DIS | SPI_CS0_DIS) &&
               MISC_CS_DIS == (SPI_CS5_DIS | SPI_CS4_DIS | SPI_CS3_DIS | SPI_CS2_DIS | SPI_CS1_DIS),
               "SPI_MISC_REG bits");
_Static_assert(GDMA_IN_CONF0_CH0_REG == GDMA_BASE + IN_CONF0 &&
               GDMA_IN_INT_RAW_CH0_REG == GDMA_BASE + IN_INT_RAW &&
               GDMA_IN_INT_CLR_CH0_REG == GDMA_BASE + IN_INT_CLR &&
               GDMA_IN_LINK_CH0_REG == GDMA_BASE + IN_LINK &&
               GDMA_OUT_CONF0_CH0_REG == GDMA_BASE + OUT_CONF0 &&
               GDMA_OUT_LINK_CH0_REG == GDMA_BASE + OUT_LINK &&
               GDMA_IN_CONF0_CH1_REG - GDMA_IN_CONF0_CH0_REG == CH_STRIDE &&
               GDMA_IN_RST_CH0 == 1u && GDMA_OUT_RST_CH0 == 1u &&
               GDMA_IN_SUC_EOF_CH0_INT_RAW == IN_SUC_EOF &&
               GDMA_IN_DSCR_ERR_CH0_INT_RAW == IN_DSCR_ERR &&
               GDMA_INLINK_START_CH0 == INLINK_START && GDMA_OUTLINK_START_CH0 == OUTLINK_START &&
               GDMA_INLINK_ADDR_CH0 == LINK_ADDR && GDMA_OUTLINK_ADDR_CH0 == LINK_ADDR, "GDMA");
_Static_assert(GDMA_IN_CONF1_CH0_REG == GDMA_BASE + IN_CONF1 &&
               GDMA_IN_INT_ENA_CH0_REG == GDMA_BASE + IN_INT_ENA &&
               GDMA_OUT_CONF1_CH0_REG == GDMA_BASE + OUT_CONF1 &&
               GDMA_OUT_INT_ENA_CH0_REG == GDMA_BASE + OUT_INT_ENA &&
               GDMA_OUT_INT_CLR_CH0_REG == GDMA_BASE + OUT_INT_CLR &&
               GDMA_INLINK_STOP_CH0 == INLINK_STOP && GDMA_OUTLINK_STOP_CH0 == OUTLINK_STOP &&
               GDMA_IN_CHECK_OWNER_CH0 == CHECK_OWNER &&
               GDMA_OUT_CHECK_OWNER_CH0 == CHECK_OWNER &&
               IN_CONF0_OFF == (GDMA_IN_LOOP_TEST_CH0 | GDMA_INDSCR_BURST_EN_CH0 |
                                GDMA_IN_DATA_BURST_EN_CH0 | GDMA_MEM_TRANS_EN_CH0) &&
               OUT_CONF0_OFF == (GDMA_OUT_LOOP_TEST_CH0 | GDMA_OUT_AUTO_WRBACK_CH0 |
                                 GDMA_OUTDSCR_BURST_EN_CH0 | GDMA_OUT_DATA_BURST_EN_CH0) &&
               GDMA_OUT_EOF_MODE_CH0 == OUT_EOF_MODE &&
               OUT_BURST == (GDMA_OUTDSCR_BURST_EN_CH0 | GDMA_OUT_DATA_BURST_EN_CH0) &&
               GDMA_OUTFIFO_STATUS_CH0_REG == GDMA_BASE + OUT_FIFO &&
               OUT_FIFO_EMPTY == (GDMA_OUTFIFO_EMPTY_L1_CH0 | GDMA_OUTFIFO_EMPTY_L2_CH0 |
                                  GDMA_OUTFIFO_EMPTY_L3_CH0),
               "GDMA channel configuration");
_Static_assert(SYSTEM_PERIP_CLK_EN0_REG == PERIP_CLK_EN0 &&
               SYSTEM_PERIP_RST_EN0_REG == PERIP_RST_EN0 && SYSTEM_SPI2_CLK_EN == SYS_SPI2 &&
               SYSTEM_SPI2_RST == SYS_SPI2, "system registers");
_Static_assert(GPIO_ENABLE_W1TS_REG == GM_ENABLE_W1TS && GPIO_ENABLE_W1TC_REG == GM_ENABLE_W1TC &&
               GPIO_ENABLE1_W1TS_REG == GM_ENABLE1_W1TS &&
               GPIO_ENABLE1_W1TC_REG == GM_ENABLE1_W1TC &&
               GPIO_FUNC0_IN_SEL_CFG_REG == GM_IN_SEL(0) &&
               GPIO_FUNC0_OUT_SEL_CFG_REG == GM_OUT_SEL(0) && GPIO_SIG0_IN_SEL == IN_SEL_MATRIX &&
               GPIO_FUNC0_OEN_SEL == OUT_OEN_SEL && SIG_GPIO_OUT_IDX == OUT_SIMPLE &&
               IO_MUX_GPIO0_REG == IOMUX(0) && IO_MUX_GPIO43_REG == IOMUX(43) &&
               FUN_IE == IOMUX_IE && FUN_PU == IOMUX_WPU && MCU_SEL_S == 12 && FUN_DRV_S == 10 &&
               PIN_FUNC_GPIO == 1, "GPIO matrix and IO MUX");
_Static_assert(FSPICLK_OUT_IDX == SIG_FSPICLK && FSPIQ_IN_IDX == SIG_FSPIQ &&
               FSPID_OUT_IDX == SIG_FSPID && FSPICS0_OUT_IDX == SIG_FSPICS0, "signal indices");

#define SLEEP_US 40      /* shorter waits spin: waking costs about 6 us (w5500.h) */
#define UPDATE_US 100    /* SPI_UPDATE takes a few clock cycles; this is a deadline */
#define CPU_FRAME_US 1000   /* 67 bytes take 54 us at 10 MHz */
#define DMA_FRAME_US 4000   /* 1,475 bytes take 1.2 ms at 10 MHz */

w5500_port_info w5500_port = {.name = "direct"};

typedef struct {         /* GDMA descriptor, TRM 3.4.1; must be in internal RAM */
    volatile uint32_t dw0;
    const volatile void *buf;
    const volatile void *next;
} gdma_desc;

static struct {
    gdma_desc desc;
    gdma_channel_handle_t tx, rx;
    intr_handle_t intr;
    StaticSemaphore_t done_buf;
    SemaphoreHandle_t done;
    uint32_t ch;             /* register base of the GDMA channel pair */
    uint32_t clock;          /* SPI_CLOCK_REG */
    uint32_t end;            /* CPU cycle count at which the transfer in flight should end */
    uint32_t cycles_per_byte;
    uint32_t len;            /* bytes of the DMA transfer in flight */
    uint32_t addr, user;     /* its SPI_ADDR and SPI_USER, for a recovery */
    bool burst;              /* its GDMA burst setting */
    unsigned rewrites;       /* of the write in flight, after underflows */
    bool recovered;          /* ... and whether GP-SPI2 and the GDMA were reset for it */
    int8_t pins[4];          /* sclk, mosi, miso, cs */
    bool claimed;            /* GP-SPI2 is ours (spicommon_bus_alloc) */
    bool bus_clock_was_on;   /* SYSTEM_SPI2_CLK_EN before we started */
    bool routed;             /* the pins are connected */
    bool busy, reading;      /* a DMA transfer is in flight */
    bool failed;             /* a transfer was aborted: frames fail until the next start */
} d;

static inline void put(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t get(uint32_t a) { return *(volatile uint32_t *)a; }

/* Spin until the mask bits of a register clear, or give up after us microseconds. The task can
 * be preempted past the deadline while the hardware finishes, so the register gets a last look. */
static bool cleared(uint32_t reg, uint32_t mask, uint32_t us)
{
    const uint32_t t0 = esp_cpu_get_cycle_count();
    const uint32_t limit = us * (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    while (get(reg) & mask)
        if (esp_cpu_get_cycle_count() - t0 > limit) return !(get(reg) & mask);
    return true;
}

/* SPI_TRANS_DONE_INT: wake the waiting task. Runs from flash; ESP-IDF masks it during flash
 * writes, and nothing waits then. */
static void on_done(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    put(G_INT_ENA, 0);
    put(G_INT_CLR, TRANS_DONE);
    xSemaphoreGiveFromISR(d.done, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* Sleep until SPI_TRANS_DONE_INT, unless the transfer ends first. */
static void sleep_done(void)
{
    xSemaphoreTake(d.done, 0);              /* drop a stale wake */
    put(G_INT_ENA, TRANS_DONE);
    if (get(G_CMD) & CMD_USR) xSemaphoreTake(d.done, pdMS_TO_TICKS(2) + 1);
    put(G_INT_ENA, 0);
    w5500_port.sleeps++;
}

static void dma_reset(uint32_t conf0)
{
    put(d.ch + conf0, get(d.ch + conf0) | 1u);    /* GDMA_*_RST_CHn: 1, then 0 (TRM 3.6.2/3.6.3) */
    put(d.ch + conf0, get(d.ch + conf0) & ~1u);
}

static void spi_reset(void)
{
    PERIPH_RCC_ATOMIC() {                   /* SYSTEM_SPI2_RST: 1, then 0 (TRM 17.3.5) */
        put(PERIP_RST_EN0, get(PERIP_RST_EN0) | SYS_SPI2);
        put(PERIP_RST_EN0, get(PERIP_RST_EN0) & ~SYS_SPI2);
    }
}

/* Take every buffer back: no SPI interrupt, both GDMA links stopped (GDMA_*LINK_STOP_CHn) and the
 * channels reset, GP-SPI2 through its peripheral reset, which ends any transaction and empties its
 * FIFOs (TRM 3.8, 17.3.5). Used after a failed wait and before the channels are released. */
static void abort_all(void)
{
    if (d.claimed) put(G_INT_ENA, 0);
    if (d.tx) {
        put(d.ch + OUT_LINK, OUTLINK_STOP);
        put(d.ch + IN_LINK, INLINK_STOP);
        dma_reset(OUT_CONF0);
        dma_reset(IN_CONF0);
    }
    if (d.claimed) spi_reset();
    d.busy = false;
}

static int fail(esp_err_t err)
{
    w5500_port.err = err;
    d.failed = true;
    abort_all();
    return 1;
}

static bool start_transfer(void);
static bool configure(void);
static void dma_configure(void);

/* Before SPI_USR: wait (up to PREFILL_US) until the GDMA holds the write's first data at every
 * FIFO level, so the SPI does not start shifting from an empty FIFO. Under the receiver's
 * internal-RAM traffic, writes started without it underran at their start: zeros, then the
 * payload's first byte repeated. */
static void prefill(void)
{
    const uint32_t t0 = esp_cpu_get_cycle_count();
    const uint32_t limit = PREFILL_US * (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    while (get(d.ch + OUT_FIFO) & OUT_FIFO_EMPTY)
        if (esp_cpu_get_cycle_count() - t0 > limit) {
            w5500_port.prefill_waits++;
            return;
        }
}

/* The payload write in flight again, from the same descriptor to the same W5500 offset (SPI_ADDR
 * and SPI_USER still hold its header and phases). */
static bool rewrite(void)
{
    put(G_DMA_CONF, DMA_TX_ENA | FIFO_RST);
    put(G_DMA_CONF, DMA_TX_ENA);
    put(d.ch + OUT_CONF0, (get(d.ch + OUT_CONF0) & ~OUT_BURST) | (d.burst ? OUT_BURST : 0));
    dma_reset(OUT_CONF0);
    put(d.ch + OUT_LINK, ((uint32_t)&d.desc & LINK_ADDR) | OUTLINK_START);
    prefill();
    if (!start_transfer()) return false;
    d.end = esp_cpu_get_cycle_count() + (3u + d.len) * d.cycles_per_byte;
    return true;
}

/* End the transfer in flight: sleep if it has long to go, then spin on SPI_USR; a read also
 * waits for GDMA_IN_SUC_EOF (its data is in memory, TRM Table 30.10-2 note 1). False if a
 * deadline passed or the GDMA reported a descriptor error (w5500_port.why says which); d.busy
 * stays set until the caller aborts, so nothing reuses the descriptor or the buffer meanwhile. */
static bool finish(void)
{
    if (!d.busy) return true;
    const int32_t left = (int32_t)(d.end - esp_cpu_get_cycle_count());
    if (left > (int32_t)(SLEEP_US * (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)) sleep_done();
    if (!cleared(G_CMD, CMD_USR, DMA_FRAME_US)) return w5500_port.why = "dma frame", false;
    /* A write whose FIFO ran dry: the GDMA fell behind (internal RAM is busy while the receiver
     * captures), and the SPI shifted out zeros and then a repeated byte. Nothing has used those
     * bytes yet (a SEND comes only after this), so they are written again. */
    if (!d.reading && (get(G_INT_RAW) & OUTFIFO_EMPTY)) {
        w5500_port.underflows++;
        if (++d.rewrites > REWRITES) {
            /* Still underrunning: GP-SPI2 and the GDMA channel from reset, as at start (no heap),
             * then once more. */
            if (d.recovered) return w5500_port.why = "underflows", false;
            d.recovered = true;
            abort_all();
            dma_configure();
            if (!configure()) return w5500_port.why = "recovery", false;
            put(G_ADDR, d.addr);
            put(G_MS_DLEN, 8u * d.len - 1u);
            put(G_USER, d.user);
            d.busy = true;
            d.rewrites = 0;
            w5500_port.recoveries++;
        }
        if (!rewrite()) return w5500_port.why = "rewrite", false;
        return finish();
    }
    d.rewrites = 0;
    d.recovered = false;
    if (d.reading) {
        const uint32_t t0 = esp_cpu_get_cycle_count();
        const uint32_t limit = DMA_FRAME_US * (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        uint32_t raw;
        while (!((raw = get(d.ch + IN_INT_RAW)) & (IN_SUC_EOF | IN_DSCR_ERR)))
            if (esp_cpu_get_cycle_count() - t0 > limit &&
                !((raw = get(d.ch + IN_INT_RAW)) & (IN_SUC_EOF | IN_DSCR_ERR)))
                return w5500_port.why = "read eof", false;
        if (raw & IN_DSCR_ERR) return w5500_port.why = "descriptor", false;
    }
    d.busy = false;
    return true;
}

/* Clear SPI_TRANS_DONE_INT, SPI_UPDATE (APB to SPI clock domain, Register 30.1), then SPI_USR. */
static bool start_transfer(void)
{
    put(G_INT_CLR, TRANS_DONE | OUTFIFO_EMPTY);
    put(G_CMD, CMD_UPDATE);
    if (!cleared(G_CMD, CMD_UPDATE, UPDATE_US)) return w5500_port.why = "update", false;
    put(G_CMD, CMD_USR);
    return true;
}

static int frame(void *ctx, uint16_t offset, uint8_t control, const uint8_t *tx, uint8_t *rx,
                 size_t n, bool post)
{
    (void)ctx;
    if (d.failed) return 1;                 /* until w5500_eth_start sets everything up again */
    if (!finish()) return fail(ESP_ERR_TIMEOUT);
    const uint8_t *buf = tx ? tx : rx;
    put(G_ADDR, w5500_gpspi_addr(offset, control));
    put(G_MS_DLEN, 8u * (uint32_t)n - 1u);
    const uint32_t user = USER_ADDR | (tx ? USER_MOSI : USER_MISO);
    if (n <= W5500_GPSPI_CPU_MAX) {
        put(G_DMA_CONF, FIFO_RST);
        put(G_DMA_CONF, 0);
        if (tx) w5500_gpspi_load((volatile uint32_t *)G_W0, tx, n);
        put(G_USER, user);
        if (!start_transfer()) return fail(ESP_ERR_TIMEOUT);
        if (!cleared(G_CMD, CMD_USR, CPU_FRAME_US)) {
            w5500_port.why = "cpu frame";
            return fail(ESP_ERR_TIMEOUT);
        }
        if (rx) w5500_gpspi_store(rx, (const volatile uint32_t *)G_W0, n);
        w5500_port.polled++;
        return 0;
    }
    if (n > W5500_GDMA_MAX || (uintptr_t)buf < DMA_RAM_LOW || (uintptr_t)buf + n > DMA_RAM_HIGH) {
        w5500_port.err = ESP_ERR_INVALID_ARG;   /* the GDMA reaches internal RAM only */
        return 1;
    }
    d.desc.dw0 = w5500_gdma_dw0((unsigned)n, tx ? (unsigned)n : 0, tx != NULL);
    d.desc.buf = buf;
    d.desc.next = NULL;
    const uint32_t ena = tx ? DMA_TX_ENA : DMA_RX_ENA;
    put(G_DMA_CONF, ena | FIFO_RST);
    put(G_DMA_CONF, ena);
    d.busy = true;                          /* the GDMA may own the buffer from here */
    d.reading = rx != NULL;
    if (tx) {
        /* Burst reads (as ESP-IDF's spi_master sets them) keep the FIFO fed while the capture
         * loads internal RAM; they need a word-aligned buffer and length (TRM 3.4.x), which TS
         * datagrams have (188 = 4 x 47). Others go a word at a time. */
        const bool burst = !(((uintptr_t)buf | n) & 3u);
        d.burst = burst;
        put(d.ch + OUT_CONF0, (get(d.ch + OUT_CONF0) & ~OUT_BURST) | (burst ? OUT_BURST : 0));
        dma_reset(OUT_CONF0);
        put(d.ch + OUT_LINK, ((uint32_t)&d.desc & LINK_ADDR) | OUTLINK_START);
        prefill();
    } else {
        dma_reset(IN_CONF0);
        put(d.ch + IN_INT_CLR, IN_SUC_EOF | IN_DSCR_ERR);
        put(d.ch + IN_LINK, ((uint32_t)&d.desc & LINK_ADDR) | INLINK_START);
    }
    put(G_USER, user);
    d.addr = w5500_gpspi_addr(offset, control);
    d.user = user;
    if (!start_transfer()) return fail(ESP_ERR_TIMEOUT);
    d.len = (uint32_t)n;
    d.rewrites = 0;
    d.recovered = false;
    d.end = esp_cpu_get_cycle_count() + (3u + d.len) * d.cycles_per_byte;
    w5500_port.queued++;
    if (post && tx) return 0;
    return finish() ? 0 : fail(ESP_ERR_TIMEOUT);
}

static bool busy(void *ctx)
{
    (void)ctx;
    return d.busy && !d.failed && (get(G_CMD) & CMD_USR);
}

static void delay(void *ctx, uint32_t us)
{
    (void)ctx;
    esp_rom_delay_us(us);   /* reset and command waits only */
}

static void pad(int pin, uint32_t bits)
{
    put(IOMUX(pin), (get(IOMUX(pin)) & ~0x7FFFu) | IOMUX_GPIO | IOMUX_DRV_2 | bits);
}

static void output(int pin, uint32_t signal)
{
    pad(pin, 0);
    put(GM_OUT_SEL(pin), signal | OUT_OEN_SEL);   /* TRM 6.5.2 */
    if (pin < 32) put(GM_ENABLE_W1TS, 1u << pin);
    else put(GM_ENABLE1_W1TS, 1u << (pin - 32));
}

static void release_output(int pin)
{
    put(GM_OUT_SEL(pin), OUT_SIMPLE);
    if (pin < 32) put(GM_ENABLE_W1TC, 1u << pin);
    else put(GM_ENABLE1_W1TC, 1u << (pin - 32));
}

/* Master, half-duplex, mode 0, 1-bit, MSB first, CS0 only, no setup/hold extension: the 0.5
 * SPI_CLK of CS setup and hold (TRM 30.6) exceeds the W5500's 5 ns (DS p.61). */
static bool configure(void)
{
    put(G_CLK_GATE, CLK_GATE_ON);
    put(G_SLAVE, 0);
    put(G_CLOCK, d.clock);
    put(G_CTRL, get(G_CTRL) & ~CTRL_MODES);
    put(G_MISC, (get(G_MISC) & ~MISC_CLEAR) | MISC_CS_DIS);
    put(G_USER, 0);
    put(G_USER1, USER1_ADDR_24);
    put(G_USER2, 0);
    put(G_DIN_MODE, 0);
    put(G_DIN_NUM, 0);
    put(G_DOUT_MODE, 0);
    put(G_DMA_CONF, 0);
    put(G_INT_ENA, 0);
    put(G_INT_CLR, 0xFFFFFFFFu);
    put(G_CMD, CMD_UPDATE);
    return cleared(G_CMD, CMD_UPDATE, UPDATE_US);
}

/* Whatever the channels' previous owner left (TRM 3.8): no burst, so size, length and buffer
 * address need no alignment (Table 3.4-2, p.364); no loop test or memory-to-memory; no owner
 * check or descriptor write-back; EOF once the last byte has left the GDMA; no interrupts. */
static void dma_configure(void)
{
    put(d.ch + IN_CONF0, get(d.ch + IN_CONF0) & ~IN_CONF0_OFF);
    put(d.ch + IN_CONF1, get(d.ch + IN_CONF1) & ~CHECK_OWNER);
    put(d.ch + OUT_CONF0, (get(d.ch + OUT_CONF0) & ~OUT_CONF0_OFF) | OUT_EOF_MODE);
    put(d.ch + OUT_CONF1, get(d.ch + OUT_CONF1) & ~CHECK_OWNER);
    put(d.ch + IN_INT_ENA, 0);
    put(d.ch + OUT_INT_ENA, 0);
    put(d.ch + IN_INT_CLR, 0xFFFFFFFFu);
    put(d.ch + OUT_INT_CLR, 0xFFFFFFFFu);
}

void w5500_port_stop(void)
{
    if (d.claimed && !d.failed && !finish()) d.failed = true;
    abort_all();
    if (d.routed) {
        /* Outputs back to inputs: the W5500's pull-up holds CS high (DS p.9). */
        release_output(d.pins[0]);
        release_output(d.pins[1]);
        release_output(d.pins[3]);
        put(GM_IN_SEL(SIG_FSPIQ), IN_SEL_MATRIX | 0x3C);   /* constant 0 (TRM 6.4.3 note 3) */
        d.routed = false;
    }
    if (d.claimed) {
        spicommon_bus_free(SPI2_HOST);
        if (!d.bus_clock_was_on) {
            PERIPH_RCC_ATOMIC() { put(PERIP_CLK_EN0, get(PERIP_CLK_EN0) & ~SYS_SPI2); }
        }
        d.claimed = false;
    }
    if (d.intr) esp_intr_free(d.intr);
    d.intr = NULL;
    if (d.tx) {
        gdma_disconnect(d.tx);
        gdma_del_channel(d.tx);
    }
    if (d.rx) {
        gdma_disconnect(d.rx);
        gdma_del_channel(d.rx);
    }
    d.tx = d.rx = NULL;
    d.busy = false;
}

esp_err_t w5500_port_start(const w5500_settings *s, w5500_bus *bus)
{
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    w5500_port = (w5500_port_info){.name = "direct"};
    uint32_t hz;
    d.clock = w5500_gpspi_clock(SPI_SRC_HZ, s->sck_khz * 1000u, &hz);
    if (!d.clock || (uintptr_t)&d.desc < DMA_RAM_LOW || (uintptr_t)&d.desc >= DMA_RAM_HIGH) {
        w5500_port.err = ESP_ERR_INVALID_ARG;
        return w5500_port.err;
    }
    if (!d.done) d.done = xSemaphoreCreateBinaryStatic(&d.done_buf);
    d.failed = false;

    /* GP-SPI2 through ESP-IDF's SPI-common allocator, then its bus clock on and a reset (TRM
     * 17.3.5). A GDMA channel pair connected to SPI2 (Table 3.4-1: peripheral 0) through ESP-IDF's
     * allocator, then configured. The SPI2 interrupt line for sleeping on TRANS_DONE. */
    esp_err_t err = spicommon_bus_alloc(SPI2_HOST, "w5500");
    if (!err) {
        d.claimed = true;
        PERIPH_RCC_ATOMIC() {
            d.bus_clock_was_on = get(PERIP_CLK_EN0) & SYS_SPI2;
            put(PERIP_CLK_EN0, get(PERIP_CLK_EN0) | SYS_SPI2);
        }
        spi_reset();
    }
    int group, channel;
    if (!err) err = gdma_new_ahb_channel(&(gdma_channel_alloc_config_t){0}, &d.tx, &d.rx);
    if (!err) err = gdma_connect(d.tx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
    if (!err) err = gdma_connect(d.rx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
    if (!err) err = gdma_get_group_channel_id(d.tx, &group, &channel);
    if (!err) {
        d.ch = GDMA_BASE + CH_STRIDE * (uint32_t)channel;
        dma_configure();
        err = esp_intr_alloc(ETS_SPI2_INTR_SOURCE, ESP_INTR_FLAG_LOWMED, on_done, NULL, &d.intr);
    }
    if (!err && !configure()) err = ESP_ERR_TIMEOUT;
    if (err) {
        w5500_port.err = err;
        w5500_port_stop();
        return err;
    }

    /* Pins through the GPIO matrix (TRM 6.4.3, 6.5.2): outputs always enabled, MISO with input
     * and a weak pull-up for when the W5500 lets it float. */
    d.pins[0] = s->sclk;
    d.pins[1] = s->mosi;
    d.pins[2] = s->miso;
    d.pins[3] = s->cs;
    output(s->cs, SIG_FSPICS0);
    output(s->sclk, SIG_FSPICLK);
    output(s->mosi, SIG_FSPID);
    pad(s->miso, IOMUX_IE | IOMUX_WPU);
    put(GM_IN_SEL(SIG_FSPIQ), IN_SEL_MATRIX | (uint32_t)s->miso);
    d.routed = true;

    d.cycles_per_byte = 8u * (uint32_t)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ull / hz) + 8u;
    d.busy = false;
    w5500_port.khz = (int)(hz / 1000u);
    w5500_port.heap_bytes = free_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *bus = (w5500_bus){.frame = frame, .delay_us = delay, .busy = busy};
    return ESP_OK;
}
