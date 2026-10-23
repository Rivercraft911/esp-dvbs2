// The receiver's transport stream out through the W5500 (eth_egress.h).
#include "eth_egress.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "w5500_eth.h"
#include "w5500_ts.h"

#define QUEUE 256U              /* packets: 47 KB of PSRAM, 36 ms at 10.39 Mb/s */
#define HOLD pdMS_TO_TICKS(10)  /* the longest a packet waits for six more */
#define DRAIN pdMS_TO_TICKS(3000)   /* at the end; a chip ARP gives up after 1.8 s (w5500.h) */
#define STACK 3072U
#define PRIORITY 10
#if CONFIG_W5500_DIRECT_SPI
#define SPI "direct"
#else
#define SPI "esp-idf"
#endif

static EXT_RAM_BSS_ATTR uint8_t slots[QUEUE * W5500_TS_PACKET] __attribute__((aligned(4)));
/* Internal RAM for the SPI DMA; also `eth send` and `eth recv`'s buffers between runs. */
static uint8_t pair[2 * W5500_TS_DATAGRAM] __attribute__((aligned(4)));

static struct {
    w5500_ts_queue q;
    w5500_ts_sender t;
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool running;      /* the sender has the chip (cleared by the sender) */
    volatile bool ending;       /* the run is over: send the rest and stop */
    bool active;                /* begun and not yet ended (the console task's view) */
    eth_egress_stats_t *stats;
} e;

static uint32_t cycles(void)
{
    return esp_cpu_get_cycle_count();
}

static void packet(void *ctx, const uint8_t *p)
{
    (void)ctx;
    w5500_ts_push(&e.q, p);
}

static void flush(void *ctx)
{
    (void)ctx;
    xTaskNotifyGive(e.task);
}

static const ts_egress_t egress = {packet, flush, NULL};

/* One run: full datagrams as they fill, the rest once it has waited HOLD or at the end, then
 * w5500_udp_poll until the chip has sent everything (bounded by DRAIN). The last error. */
static int pump(void)
{
    w5500 *w = w5500_eth();
    TickType_t since = 0, end = 0;
    bool waiting = false;       /* fewer than seven packets wait */
    int staged = 0;             /* datagrams in the chip not yet sent */
    for (;;) {
        const bool ending = e.ending;   /* read first: every packet was pushed before it was set */
        const uint32_t before = e.t.st.datagrams;
        int r = w5500_ts_send(&e.t, false);
        const TickType_t now = xTaskGetTickCount();
        if (e.t.st.datagrams != before) waiting = false;
        if (r == W5500_OK && w5500_ts_waiting(&e.q)) {
            if (!waiting) since = now;
            waiting = true;
            if (ending || now - since >= HOLD) {
                r = w5500_ts_send(&e.t, true);
                waiting = false;
            }
        }
        if (r < 0) return r;
        if (r == W5500_FULL || e.t.st.datagrams != before || staged > 0)
            staged = w5500_udp_poll(w, 0);   /* starts the last datagram's SEND */
        if (staged < 0) return staged;
        if (ending) {
            if (!staged && !e.t.held && !w5500_ts_waiting(&e.q)) return W5500_OK;
            if (!end) end = now | 1;
            if (now - end > DRAIN) return W5500_FULL;
        }
        const bool busy = waiting || staged > 0 || e.t.held || ending;
        ulTaskNotifyTake(pdTRUE, busy ? 1 : portMAX_DELAY);
    }
}

static void sender(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!e.running) continue;
        eth_egress_stats_t *s = e.stats;
        w5500 *w = w5500_eth();
        s->status = w5500_eth_start(W5500_ETH_SEND);
        if (s->status) {
            e.running = false;
            xSemaphoreGive(e.done);
            continue;
        }
        s->phy = w5500_phy(w);
        uint8_t *const buf[2] = {pair, pair + W5500_TS_DATAGRAM};
        w5500_ts_sender_init(&e.t, w, 0, &e.q, buf, cycles);
        xSemaphoreGive(e.done);
        s->status = pump();
        const w5500_stats *st = w5500_udp_stats(w, 0);
        s->sent = st->sent;
        s->arp_dropped = st->dropped;
        w5500_eth_stop();
        s->stack_free = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        e.running = false;
        xSemaphoreGive(e.done);
    }
}

void eth_egress_init(void)
{
    static StaticSemaphore_t done;
    static StackType_t stack[STACK / sizeof(StackType_t)];
    static StaticTask_t task;
    e.done = xSemaphoreCreateBinaryStatic(&done);
    e.task = xTaskCreateStaticPinnedToCore(sender, "eth", STACK, NULL, PRIORITY, stack, &task, 0);
    w5500_eth_reserve(pair);    /* a failure shows at the first start */
}

const ts_egress_t *eth_egress_begin(eth_egress_stats_t *s)
{
    memset(s, 0, sizeof *s);
    s->phy = -1;
    s->queue = QUEUE;
    w5500_ts_queue_init(&e.q, slots, QUEUE);
    e.stats = s;
    e.ending = false;
    e.running = true;
    xTaskNotifyGive(e.task);
    xSemaphoreTake(e.done, portMAX_DELAY);
    e.active = !s->status;
    return e.active ? &egress : NULL;
}

void eth_egress_end(eth_egress_stats_t *s)
{
    if (!e.active) return;      /* never started */
    e.active = false;
    e.ending = true;
    xTaskNotifyGive(e.task);
    xSemaphoreTake(e.done, portMAX_DELAY);   /* given once per run, also after an early error */
    const w5500_ts_stats *t = &e.t.st;
    s->datagrams = t->datagrams;
    s->packets = t->packets;
    s->partial = t->partial;
    s->full = t->full;
    s->max_send_cycles = t->max_send_cycles;
    s->send_cycles = t->send_cycles;
    s->dropped = e.q.dropped;
    s->peak = e.q.peak;
}

void eth_egress_print(const eth_egress_stats_t *s)
{
    printf(",\"eth\":{\"status\":\"%s\",\"spi\":\"" SPI "\"", w5500_eth_status(s->status));
    if (s->phy >= 0)
        printf(",\"link\":%s,\"link_mbps\":%d", s->phy & W5500_PHY_LINK ? "true" : "false",
               s->phy & W5500_PHY_100M ? 100 : 10);
    printf(",\"datagrams\":%lu,\"sent\":%lu,\"packets\":%lu,\"partial\":%lu,\"dropped\":%lu,"
           "\"peak\":%lu,\"queue\":%lu,\"full\":%lu,\"arp_dropped\":%lu,\"max_send_cycles\":%lu,"
           "\"send_cycles\":%llu,\"stack_free\":%lu}",
           (unsigned long)s->datagrams, (unsigned long)s->sent, (unsigned long)s->packets,
           (unsigned long)s->partial, (unsigned long)s->dropped, (unsigned long)s->peak,
           (unsigned long)s->queue, (unsigned long)s->full, (unsigned long)s->arp_dropped,
           (unsigned long)s->max_send_cycles, (unsigned long long)s->send_cycles,
           (unsigned long)s->stack_free);
}
