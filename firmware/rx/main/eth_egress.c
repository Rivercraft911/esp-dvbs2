// The receiver's transport stream out through the W5500 (eth_egress.h).
#include "eth_egress.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "w5500_eth.h"
#include "w5500_ts.h"

#define QUEUE 4096U             /* packets: 770 KB of PSRAM, 0.74 s at 8.3 Mb/s */
#define HOLD pdMS_TO_TICKS(10)  /* the longest a packet waits for six more */
#define DRAIN pdMS_TO_TICKS(3000)   /* at the end; a chip ARP gives up after 1.8 s (w5500.h) */
#define SAT_US 20000            /* a SEND unfinished this long, 18 wire times: give it again */
#define STACK 3072U
#define PRIORITY 13             /* above the release (12): see eth_egress.h */
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
    esp_timer_handle_t kick;    /* wakes the sender when the datagram on the wire should be out */
    unsigned link_mbps;
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

static void kick(void *arg)
{
    (void)arg;
    xTaskNotifyGive(e.task);
}

/* The chip sends one datagram at a time and starts the next only when the driver is called
 * (w5500.h). At 10 Mb/s a 1,316-byte datagram is 1.106 ms on the wire, longer than a tick, so
 * the sender is woken when it should be done: its bytes plus 66 of UDP, IP, Ethernet, preamble
 * and gap, at the link rate, from when its SEND was given. */
typedef struct {
    uint16_t wr;                /* Sn_TX_WR at the last SEND seen */
    uint32_t started;           /* CPU cycle count just after the call that gave it */
} send_clock;

/* After every driver call: a new SEND moved Sn_TX_WR. With a posting backend a later call first
 * waits out a payload write (about 400 us), so the sender notes it inside w5500_ts_send. */
static void note_send(const w5500_socket *so, send_clock *c)
{
    if (e.t.wr != c->wr && e.t.wr == so->tx_wr) {
        c->wr = e.t.wr;
        c->started = e.t.wr_at;
    } else if (so->tx_wr != c->wr) {
        c->wr = so->tx_wr;
        c->started = esp_cpu_get_cycle_count();
    }
}

static void wake_after_send(const w5500_socket *so, const send_clock *c)
{
    if (!so->flight) return;
    const int32_t since = (int32_t)(esp_cpu_get_cycle_count() - c->started) /
                          CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    int64_t due = (so->flight + 66) * 8 / e.link_mbps + 10 - since;
    if (due < 20) due = 20;
    esp_timer_stop(e.kick);
    esp_timer_start_once(e.kick, (uint64_t)due);
}

/* One run: full datagrams as they fill, the rest once it has waited HOLD or at the end, then
 * w5500_udp_poll until the chip has sent everything (bounded by DRAIN). The last error. */
static int pump(void)
{
    w5500 *w = w5500_eth();
    TickType_t since = 0, end = 0;
    bool waiting = false;       /* fewer than seven packets wait */
    int staged = 0;             /* datagrams in the chip not yet sent */
    /* Drain rate: datagrams sent while at least two datagrams' worth waited, over that time. */
    bool backlog = false;
    int64_t mark = esp_timer_get_time();
    uint32_t marked = 0;
    uint32_t sent = w->sock[0].st.sent;   /* the chip's last progress, and when */
    uint16_t sent_wr = w->sock[0].tx_wr;
    int64_t moved = mark;
    send_clock clock = {w->sock[0].tx_wr, 0};
    for (;;) {
        const bool ending = e.ending;   /* read first: every packet was pushed before it was set */
        const uint32_t before = e.t.st.datagrams;
        const int64_t t = esp_timer_get_time();
        if (backlog) {
            e.stats->backlog_us += t - mark;
            e.stats->backlog_datagrams += before - marked;
        }
        backlog = w5500_ts_waiting(&e.q) >= 2 * W5500_TS_GROUP;
        mark = t;
        marked = before;
        int r = w5500_ts_send(&e.t, false);
        note_send(&w->sock[0], &clock);
        const TickType_t now = xTaskGetTickCount();
        if (e.t.st.datagrams != before) waiting = false;
        if (r == W5500_OK && w5500_ts_waiting(&e.q)) {
            if (!waiting) since = now;
            waiting = true;
            if (ending || now - since >= HOLD) {
                r = w5500_ts_send(&e.t, true);
                note_send(&w->sock[0], &clock);
                waiting = false;
            }
        }
        if (r < 0) return r;
        if (r == W5500_FULL || e.t.st.datagrams != before || staged > 0) {
            staged = w5500_udp_poll(w, 0);   /* starts the last datagram's SEND */
            note_send(&w->sock[0], &clock);
        }
        if (staged < 0) return staged;
        const int64_t now_us = esp_timer_get_time();
        if (w->sock[0].st.sent != sent || w->sock[0].tx_wr != sent_wr) {
            sent = w->sock[0].st.sent;
            sent_wr = w->sock[0].tx_wr;
            moved = now_us;
        } else if (staged > 0 && now_us - moved > SAT_US) {
            const int again = w5500_udp_resend(w, 0);   /* w5500.h: the chip sat on it */
            if (again < 0) return again;
            if (again) clock.started = esp_cpu_get_cycle_count();
            moved = now_us;
        }
        if (staged > 0) wake_after_send(&w->sock[0], &clock);
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
        const uint32_t underflows = w5500_eth_underflows(), recoveries = w5500_eth_recoveries();
        uint8_t *const buf[2] = {pair, pair + W5500_TS_DATAGRAM};
        w5500_ts_sender_init(&e.t, w, 0, &e.q, buf, cycles);
        xSemaphoreGive(e.done);
        e.link_mbps = s->phy >= 0 && !(s->phy & W5500_PHY_100M) ? 10 : 100;
        s->status = pump();
        esp_timer_stop(e.kick);
        const w5500_socket *so = &w->sock[0];
        s->stuck = s->status != W5500_OK && w5500_udp_regs(w, 0, &s->regs) == W5500_OK;
        s->flight = so->flight;
        s->staged = so->q_count;
        s->tx_wr = so->tx_wr;
        s->tx_end = so->tx_end;
        s->cmd_pending = so->cmd_pending;
        s->esp_err = w5500_eth_error();
        s->underflows = w5500_eth_underflows() - underflows;
        s->recoveries = w5500_eth_recoveries() - recoveries;
        const w5500_stats *st = w5500_udp_stats(w, 0);
        s->sent = st->sent;
        s->resends = st->resends;
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
    esp_timer_create(&(esp_timer_create_args_t){.callback = kick, .name = "eth"}, &e.kick);
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
    if (s->status == W5500_ERR_BUS)
        printf(",\"esp_err\":\"%s\",\"spi_wait\":\"%s\"", esp_err_to_name(s->esp_err),
               w5500_eth_why());
    if (s->phy >= 0)
        printf(",\"link\":%s,\"link_mbps\":%d", s->phy & W5500_PHY_LINK ? "true" : "false",
               s->phy & W5500_PHY_100M ? 100 : 10);
    printf(",\"backlog_ms\":%.1f,\"backlog_datagrams\":%lu,\"drain_mbps\":%.3f",
           s->backlog_us / 1000.0, (unsigned long)s->backlog_datagrams,
           s->backlog_us ? s->backlog_datagrams * 8.0 * W5500_TS_DATAGRAM / s->backlog_us : 0.0);
    printf(",\"resends\":%lu,\"recoveries\":%lu", (unsigned long)s->resends,
           (unsigned long)s->recoveries);
    printf(",\"datagrams\":%lu,\"sent\":%lu,\"packets\":%lu,\"partial\":%lu,\"dropped\":%lu,"
           "\"peak\":%lu,\"queue\":%lu,\"full\":%lu,\"arp_dropped\":%lu,\"max_send_cycles\":%lu,"
           "\"send_cycles\":%llu,\"stack_free\":%lu,\"underflows\":%lu",
           (unsigned long)s->datagrams, (unsigned long)s->sent, (unsigned long)s->packets,
           (unsigned long)s->partial, (unsigned long)s->dropped, (unsigned long)s->peak,
           (unsigned long)s->queue, (unsigned long)s->full, (unsigned long)s->arp_dropped,
           (unsigned long)s->max_send_cycles, (unsigned long long)s->send_cycles,
           (unsigned long)s->stack_free, (unsigned long)s->underflows);
    if (s->stuck)
        printf(",\"stuck\":{\"sn_cr\":%u,\"sn_ir\":%u,\"sn_sr\":%u,\"sn_tx_fsr\":%u,"
               "\"sn_tx_rd\":%u,\"sn_tx_wr\":%u,\"flight\":%u,\"staged\":%u,\"tx_wr\":%u,"
               "\"tx_end\":%u,\"cmd_pending\":%s}", s->regs.cr, s->regs.ir, s->regs.sr,
               s->regs.tx_fsr, s->regs.tx_rd, s->regs.tx_wr, s->flight, s->staged, s->tx_wr,
               s->tx_end, s->cmd_pending ? "true" : "false");
    printf("}");
}
