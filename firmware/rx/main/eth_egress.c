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

#include "mon_pack.h"
#include "run_arena.h"
#include "w5500_eth.h"
#include "w5500_ts.h"

#define QUEUE 4096U             /* packets: 770 KB of PSRAM, 0.74 s at 8.3 Mb/s */
#define HOLD pdMS_TO_TICKS(10)  /* the longest a packet waits for six more */
#define DRAIN pdMS_TO_TICKS(3000)   /* at the end; a chip ARP gives up after 1.8 s (w5500.h) */
#define SAT_US 20000            /* a SEND unfinished this long, 18 wire times: give it again */
#define STACK 3072U
#define PRIORITY 13             /* below the release (14), above the checker: eth_egress.h */
#if CONFIG_W5500_DIRECT_SPI
#define SPI "direct"
#else
#define SPI "esp-idf"
#endif

static EXT_RAM_BSS_ATTR uint8_t slots[QUEUE * W5500_TS_PACKET] __attribute__((aligned(4)));
/* Internal RAM for the SPI DMA; also `eth send` and `eth recv`'s buffers between runs. */
static uint8_t pair[2 * W5500_TS_DATAGRAM] __attribute__((aligned(4)));

/* The monitor stream: snapshots in PSRAM, one internal datagram buffer for the SPI DMA. */
#define MON_SOCK 2
/* Rates per second (`mon`), 0 for off: sample snapshots, symbol taps, stats records. */
static unsigned mon_hz[3] = {10, 10, 5};
#define MON_MER 50              /* the last frame's MER every 20 ms: the last second */
/* Internal RAM: core 1 copies into these after a unit, and a PSRAM write through the cache there
 * cost the receiver frames (core 1 has a few percent of slack at 5 Msym/s). */
static int8_t mon_si[MON_SNAPSHOT], mon_sq[MON_SNAPSHOT];
static EXT_RAM_BSS_ATTR int8_t mon_sym[2 * MON_SYMBOLS_MAX];
static uint8_t mon_buf[MON_HEADER + MON_STATS_MAX] __attribute__((aligned(4)));
static rx_monitor_t monitor = {.si = mon_si, .sq = mon_sq};

static struct {
    bool on;
    eth_run_t run;
    uint32_t runs;
    int64_t next_samples, next_symbols, next_stats, next_mer;
    uint16_t seq[4], snapshot;
    unsigned chunk;             /* the next of the snapshot's chunks to send */
    bool tap_armed, stats_due;
    int16_t mer[MON_MER];
    unsigned mer_n, mer_at;
    uint32_t mer_frames;
    int64_t mon_done;           /* when the monitor datagram in flight should be out */
} ms;

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

/* The wire's schedule as the sender sees it: the chip sends one frame at a time, each SEND as
 * soon as the frame before has left, so a SEND given at t leaves at max(t, the last one's end)
 * plus its bytes and 66 more (UDP, IP, Ethernet, preamble, gap) at the link rate. */
typedef struct {
    uint16_t wr[2];             /* each socket's Sn_TX_WR at its last SEND seen */
    int64_t done[2];            /* when that SEND should be out, us */
    int64_t end;                /* when the last SEND should be out */
} wire_t;

static void track(const w5500 *w, wire_t *m)
{
    const int64_t now = esp_timer_get_time();
    for (unsigned s = 0; s < 2; ++s) {
        const w5500_socket *so = &w->sock[s];
        if (so->tx_wr == m->wr[s]) continue;
        m->wr[s] = so->tx_wr;
        m->end = (m->end > now ? m->end : now) + (so->flight + 66) * 8 / e.link_mbps;
        m->done[s] = m->end;
    }
}

static bool busy_socket(const w5500 *w, unsigned s)
{
    return w->sock[s].flight || w->sock[s].q_count;
}

static void mon_stats_now(mon_stats_t *st)
{
    rx_demod_t *d = monitor.demod;
    rx_ts_t *ts = monitor.ts;
    const receive_stats_t *rs = ms.run.rx;
    *st = (mon_stats_t){.run = ms.runs, .freq_mhz = (uint16_t)ms.run.mhz,
                        .symbol_rate = ms.run.rate, .modcod = (uint8_t)ms.run.modcod,
                        .gain = (uint8_t)ms.run.gain, .core0_pmil = -1, .core1_pmil = -1};
    if (d) {
        rx_demod_stats_t ds;
        rx_demod_stats(d, &ds);
        st->track = ds.locked;
        st->lost = ds.lost;
        st->runs_dropped = ds.dropped;
        st->relocks = ds.relocks;
        st->dummies = ds.dummies;
    }
    if (rs) {
        st->frames = rs->good;
        st->failed = rs->failed;
        st->ldpc = rs->decoded;
        st->frames_dropped = rs->dropped;
    }
    if (ms.mer_n) {
        int16_t v[MON_MER];
        memcpy(v, ms.mer, sizeof v);
        st->have_mer = true;
        st->mer_cdb = monitor.mer_cdb;
        st->mer_med_cdb = mon_median(v, ms.mer_n);
        st->cfo_hz = monitor.cfo_hz;
        st->clock_ppb = monitor.clock_ppb;
    }
    if (ts) {
        rx_ts_stats_t t;
        rx_ts_stats(ts, &t);
        st->ts = true;
        st->packets = t.packets;
        st->crc8 = t.crc_errors;
        st->cc = t.cc_errors;
        st->tei = t.crc_errors;     /* the deframer marks exactly the CRC-8 failures */
    }
    st->eth = true;
    st->eth_sent = e.t.st.datagrams;
    st->eth_dropped = e.q.dropped;
    st->eth_q_peak = e.q.peak;
    st->mon_dropped = e.stats->mon_dropped;
    if (ms.run.capture) st->capture_drops = ms.run.capture->drops;
    arena_heap_t h;
    arena_heap(&h);
    st->heap_after_boot = h.allocs;
}

/* The monitor's turn, after the TS: schedules, then at most one datagram on socket 2, and only
 * when no TS datagram waits. What is not sent within its period is dropped and counted. */
static int mon_step(w5500 *w)
{
    if (!ms.on) return 0;
    eth_egress_stats_t *s = e.stats;
    const int64_t now = esp_timer_get_time();
    if (now >= ms.next_mer) {
        ms.next_mer = now + 20000;
        if (monitor.frames != ms.mer_frames) {
            ms.mer_frames = monitor.frames;
            ms.mer[ms.mer_at] = monitor.mer_cdb;
            ms.mer_at = (ms.mer_at + 1) % MON_MER;
            if (ms.mer_n < MON_MER) ms.mer_n++;
        }
    }
    if (mon_hz[0] && now >= ms.next_samples) {
        ms.next_samples = now + 1000000 / mon_hz[0];
        if (monitor.have_samples && ms.chunk < MON_CHUNKS) s->mon_dropped += MON_CHUNKS - ms.chunk;
        monitor.have_samples = false;   /* core 1 overwrites the planes only after this */
        ms.chunk = 0;
        ms.snapshot++;
        monitor.want_samples = true;
    }
    if (mon_hz[1] && now >= ms.next_symbols) {
        ms.next_symbols = now + 1000000 / mon_hz[1];
        rx_demod_t *d = monitor.demod;
        if (d && ms.tap_armed && rx_demod_tapped(d)) s->mon_dropped++;   /* filled, unsent */
        if (d && (!ms.tap_armed || rx_demod_tapped(d))) {   /* never while the release fills */
            rx_demod_tap(d, mon_sym, MON_SYMBOLS_MAX);
            ms.tap_armed = true;
        }
    }
    if (mon_hz[2] && now >= ms.next_stats) {
        ms.next_stats = now + 1000000 / mon_hz[2];
        if (ms.stats_due) s->mon_dropped++;
        ms.stats_due = true;
    }
    if (w5500_ts_waiting(&e.q) >= W5500_TS_GROUP || e.t.held) return 0;   /* TS first */
    if (busy_socket(w, MON_SOCK)) {
        if (now < ms.mon_done) return 0;    /* still going: no SPI frames to find that out */
        const int p = w5500_udp_poll(w, MON_SOCK);
        if (p) return p < 0 ? p : 0;
    }
    const uint32_t t0 = esp_cpu_get_cycle_count();
    const uint32_t t_us = (uint32_t)now;
    size_t n = 0;
    rx_demod_t *d = monitor.demod;
    if (ms.stats_due) {
        mon_stats_t st;
        mon_stats_now(&st);
        n = mon_stats(mon_buf, ms.seq[MON_STATS]++, t_us, &st);
        ms.stats_due = false;
    } else if (monitor.have_samples && ms.chunk < MON_CHUNKS) {
        n = mon_samples_planes(mon_buf, ms.seq[MON_SAMPLES]++, t_us, ms.snapshot, ms.chunk++,
                               mon_si, mon_sq);
    } else if (d && ms.tap_armed && rx_demod_tapped(d)) {
        n = mon_symbols(mon_buf, ms.seq[MON_SYMBOLS]++, t_us, MON_SYMBOLS_MAX,
                        ms.run.modcod >= 12 ? 3 : 2, mon_sym);
        ms.tap_armed = false;
    }
    if (!n) return 0;
    const int r = w5500_udp_send(w, MON_SOCK, mon_buf, n);
    const int p = r == W5500_OK ? w5500_udp_poll(w, MON_SOCK) : 0;   /* SEND now; mon_buf free */
    const uint32_t c = esp_cpu_get_cycle_count() - t0;
    s->mon_cycles += c;
    if (c > s->mon_max_cycles) s->mon_max_cycles = c;
    if (r < 0 || p < 0) return r < 0 ? r : p;
    if (r == W5500_FULL) {
        s->mon_dropped++;
    } else {
        s->mon_sent++;
        ms.mon_done = now + 2 * ((int64_t)n + 66) * 8 / e.link_mbps;   /* behind a TS datagram */
    }
    return 0;
}

/* One run on sockets 0 and 1 in turn (w5500_ts.h): full datagrams as they fill, the rest once
 * it has waited HOLD or at the end, then w5500_udp_poll until the chip has sent everything
 * (bounded by DRAIN). The sender wakes on each frame and, by the kick, when the socket it waits
 * for should be free; the other socket's datagram keeps the wire busy meanwhile. The last
 * error. */
static int pump(void)
{
    w5500 *w = w5500_eth();
    TickType_t since = 0, end = 0;
    bool waiting = false;       /* fewer than seven packets wait */
    int staged = 0;             /* sockets with a datagram not yet out */
    /* Drain rate: datagrams sent while at least two datagrams' worth waited, over that time. */
    bool backlog = false;
    int64_t mark = esp_timer_get_time();
    uint32_t marked = 0;
    wire_t m = {{w->sock[0].tx_wr, w->sock[1].tx_wr}, {0, 0}, 0};
    uint32_t sent[2] = {w->sock[0].st.sent, w->sock[1].st.sent};   /* progress, per socket */
    uint16_t sent_wr[2] = {w->sock[0].tx_wr, w->sock[1].tx_wr};
    int64_t moved[2] = {mark, mark};
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
        track(w, &m);
        const TickType_t now = xTaskGetTickCount();
        if (e.t.st.datagrams != before) waiting = false;
        if (r == W5500_OK && w5500_ts_waiting(&e.q)) {
            if (!waiting) since = now;
            waiting = true;
            if (ending || now - since >= HOLD) {
                r = w5500_ts_send(&e.t, true);
                track(w, &m);
                waiting = false;
            }
        }
        if (r < 0) return r;
        const int mr = mon_step(w);
        if (mr < 0) return mr;
        if (r == W5500_OK) {            /* nothing left to give: let the chip finish */
            const int64_t at = esp_timer_get_time();
            for (unsigned s = 0; s < 2; ++s) {
                /* Only once its datagram should be out: each poll is SPI frames, and this runs
                 * on every wake. */
                if (!busy_socket(w, s) || (at < m.done[s] && !ending)) continue;
                const int p = w5500_udp_poll(w, s);
                if (p < 0) return p;
            }
            track(w, &m);
        }
        staged = busy_socket(w, 0) + busy_socket(w, 1);
        const int64_t now_us = esp_timer_get_time();
        int64_t wake = INT64_MAX;
        for (unsigned s = 0; s < 2; ++s) {
            if (w->sock[s].st.sent != sent[s] || w->sock[s].tx_wr != sent_wr[s]) {
                sent[s] = w->sock[s].st.sent;
                sent_wr[s] = w->sock[s].tx_wr;
                moved[s] = now_us;
            } else if (busy_socket(w, s) && now_us - moved[s] > SAT_US) {
                const int again = w5500_udp_resend(w, s);   /* w5500.h: the chip sat on it */
                if (again < 0) return again;
                if (again) m.done[s] = now_us + (w->sock[s].flight + 66) * 8 / e.link_mbps;
                moved[s] = now_us;
            }
            /* Wake for the socket the next datagram waits for, or, when all is given, for the
             * first to finish. */
            const bool wanted = r == W5500_FULL ? s == e.t.next : busy_socket(w, s);
            if (wanted && busy_socket(w, s) && m.done[s] < wake) wake = m.done[s];
        }
        if (wake != INT64_MAX) {
            int64_t due = wake + 10 - now_us;
            if (due < 20) due = 20;
            esp_timer_stop(e.kick);
            esp_timer_start_once(e.kick, (uint64_t)due);
        }
        if (ending) {
            if (!staged && !e.t.held && !w5500_ts_waiting(&e.q)) return W5500_OK;
            if (!end) end = now | 1;
            if (now - end > DRAIN) return W5500_FULL;
        }
        const bool busy = waiting || staged > 0 || e.t.held || ending;
        /* The monitor needs a turn now and then even without frames (acquiring): every 20 ms. */
        ulTaskNotifyTake(pdTRUE, busy ? 1 : ms.on ? pdMS_TO_TICKS(20) : portMAX_DELAY);
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
        static const unsigned socks[2] = {0, 1};
        w5500_ts_sender_init(&e.t, w, socks, 2, &e.q, buf, cycles);
        ms.on = w->sock[MON_SOCK].open;
        s->mon = ms.on;
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
        for (unsigned k = 0; k < 2; ++k) {
            const w5500_stats *st = w5500_udp_stats(w, k);
            s->sent += st->sent;
            s->resends += st->resends;
            s->arp_dropped += st->dropped;
        }
        w5500_eth_stop();
        s->stack_free = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        s->mon_copies = monitor.copies;
        s->mon_copy_cycles = monitor.copy_cycles;
        s->mon_copy_max = monitor.copy_max;
        ms.on = false;
        e.running = false;
        xSemaphoreGive(e.done);
    }
}

void eth_egress_monitor_rates(unsigned samples, unsigned symbols, unsigned stats)
{
    mon_hz[0] = samples > 10 ? 10 : samples;
    mon_hz[1] = symbols > 10 ? 10 : symbols;
    mon_hz[2] = stats > 10 ? 10 : stats;
    printf("@{\"cmd\":\"mon\",\"samples_hz\":%u,\"symbols_hz\":%u,\"stats_hz\":%u}\n",
           mon_hz[0], mon_hz[1], mon_hz[2]);
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

const ts_egress_t *eth_egress_begin(eth_egress_stats_t *s, const eth_run_t *run,
                                    rx_monitor_t **mon)
{
    memset(s, 0, sizeof *s);
    const int64_t now = esp_timer_get_time();
    ms = (typeof(ms)){.run = *run, .runs = ms.runs + 1, .next_samples = now, .next_symbols = now,
                      .next_stats = now, .next_mer = now, .chunk = MON_CHUNKS};
    monitor = (rx_monitor_t){.si = mon_si, .sq = mon_sq};
    s->phy = -1;
    s->queue = QUEUE;
    w5500_ts_queue_init(&e.q, slots, QUEUE);
    e.stats = s;
    e.ending = false;
    e.running = true;
    xTaskNotifyGive(e.task);
    xSemaphoreTake(e.done, portMAX_DELAY);
    e.active = !s->status;
    *mon = e.active && s->mon ? &monitor : NULL;
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
    if (s->mon) {
        const uint32_t sent = s->mon_sent ? s->mon_sent : 1, copies = s->mon_copies ? s->mon_copies : 1;
        printf(",\"mon\":{\"sent\":%lu,\"dropped\":%lu,\"copies\":%lu,\"copy_cycles\":%llu,"
               "\"copy_max\":%lu,\"cycles_per_datagram\":%llu,\"max_cycles\":%lu}",
               (unsigned long)s->mon_sent, (unsigned long)s->mon_dropped,
               (unsigned long)s->mon_copies, (unsigned long long)(s->mon_copy_cycles / copies),
               (unsigned long)s->mon_copy_max, (unsigned long long)(s->mon_cycles / sent),
               (unsigned long)s->mon_max_cycles);
    }
    if (s->stuck)
        printf(",\"stuck\":{\"sn_cr\":%u,\"sn_ir\":%u,\"sn_sr\":%u,\"sn_tx_fsr\":%u,"
               "\"sn_tx_rd\":%u,\"sn_tx_wr\":%u,\"flight\":%u,\"staged\":%u,\"tx_wr\":%u,"
               "\"tx_end\":%u,\"cmd_pending\":%s}", s->regs.cr, s->regs.ir, s->regs.sr,
               s->regs.tx_fsr, s->regs.tx_rd, s->regs.tx_wr, s->flight, s->staged, s->tx_wr,
               s->tx_end, s->cmd_pending ? "true" : "false");
    printf("}");
}
