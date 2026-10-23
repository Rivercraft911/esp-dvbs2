/* ESP-IDF side of the W5500 driver: ownership, MAC, settings, `eth` (w5500_eth.h). The SPI is
 * in w5500_spi_idf.c or w5500_spi_direct.c (w5500_port.h). */
#include "w5500_eth.h"
#include "w5500_port.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#if CONFIG_W5500_SETTINGS_NVS
#include "nvs.h"
#include "nvs_flash.h"
#endif

#define TS_PACKET 188
#define DATAGRAM (7 * TS_PACKET)
#define STORE_VERSION 1

static w5500 chip;
static w5500_bus bus;
static bool port_up;          /* w5500_port_start has run and w5500_port_stop has not */
static bool reserved;         /* the port stays up between owners (w5500_eth_reserve) */
static uint8_t *lent;         /* the app's two diagnostic buffers, or NULL for the heap */
static w5500_settings port_settings;   /* what the port was started with */

/* One task owns the chip from w5500_eth_start to w5500_eth_stop; the driver and the shared
 * transaction above are not reentrant. Others, such as the console, get W5500_ERR_BUSY. */
static StaticSemaphore_t owner_mutex;
static SemaphoreHandle_t owner_lock;
static TaskHandle_t owner;
static portMUX_TYPE owner_init = portMUX_INITIALIZER_UNLOCKED;

static bool own(void)
{
    if (owner == xTaskGetCurrentTaskHandle()) return true;
    taskENTER_CRITICAL(&owner_init);
    if (!owner_lock) owner_lock = xSemaphoreCreateMutexStatic(&owner_mutex);
    taskEXIT_CRITICAL(&owner_init);
    if (xSemaphoreTake(owner_lock, 0) != pdTRUE) return false;
    owner = xTaskGetCurrentTaskHandle();
    return true;
}

static void disown(void)
{
    owner = NULL;
    xSemaphoreGive(owner_lock);
}

w5500 *w5500_eth(void)
{
    return &chip;
}

esp_err_t w5500_eth_error(void)
{
    return w5500_port.err;
}

/* The settings: the built-in defaults, then CONFIG_W5500_DEFAULTS (settings separated by ';'),
 * then with CONFIG_W5500_SETTINGS_NVS what `eth` saved; read once, then kept in RAM. */
static w5500_settings settings;
static bool loaded;

static void app_defaults(w5500_settings *s)
{
    char text[] = CONFIG_W5500_DEFAULTS, *save;
    w5500_settings_default(s);
    for (char *one = strtok_r(text, ";", &save); one; one = strtok_r(NULL, ";", &save))
        w5500_settings_parse(s, one);
}

#if CONFIG_W5500_SETTINGS_NVS
typedef struct {
    uint8_t version;
    w5500_settings s;
} stored_settings;

/* Open the settings namespace, mounting NVS only for the moment if the app has not. */
static esp_err_t open_store(nvs_open_mode_t mode, nvs_handle_t *h, bool *mounted)
{
    *mounted = false;
    esp_err_t err = nvs_open("w5500", mode, h);
    if (err != ESP_ERR_NVS_NOT_INITIALIZED) return err;
    if ((err = nvs_flash_init()) != ESP_OK) return err;
    *mounted = true;
    if ((err = nvs_open("w5500", mode, h)) != ESP_OK) nvs_flash_deinit();
    return err;
}

static void close_store(nvs_handle_t h, bool mounted)
{
    nvs_close(h);
    if (mounted) nvs_flash_deinit();
}

static void load(w5500_settings *s)
{
    stored_settings stored;
    size_t size = sizeof stored;
    nvs_handle_t h;
    bool mounted;
    if (open_store(NVS_READONLY, &h, &mounted) != ESP_OK) return;
    if (nvs_get_blob(h, "settings", &stored, &size) == ESP_OK && size == sizeof stored &&
        stored.version == STORE_VERSION && w5500_settings_valid(&stored.s)) *s = stored.s;
    close_store(h, mounted);
}

static esp_err_t save(const w5500_settings *s)
{
    stored_settings stored;
    memset(&stored, 0, sizeof stored);
    stored.version = STORE_VERSION;
    stored.s = *s;
    nvs_handle_t h;
    bool mounted;
    esp_err_t err = open_store(NVS_READWRITE, &h, &mounted);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "settings", &stored, sizeof stored);
    if (err == ESP_OK) err = nvs_commit(h);
    close_store(h, mounted);
    return err;
}
#else
static void load(w5500_settings *s)
{
    (void)s;
}
#endif

void w5500_eth_settings(w5500_settings *s)
{
    if (!loaded) {
        app_defaults(&settings);
        load(&settings);
        loaded = true;
    }
    *s = settings;
}

/* The SPI pins and clock the port runs with. */
static bool same_port(const w5500_settings *a, const w5500_settings *b)
{
    return a->sclk == b->sclk && a->mosi == b->mosi && a->miso == b->miso && a->cs == b->cs &&
           a->sck_khz == b->sck_khz;
}

static esp_err_t port_start(const w5500_settings *s)
{
    const esp_err_t err = w5500_port_start(s, &bus);
    port_up = err == ESP_OK;
    port_settings = *s;
    return err;
}

static void port_stop(void)
{
    if (port_up) w5500_port_stop();
    port_up = false;
}

int w5500_eth_reserve(uint8_t *buffers)
{
    w5500_settings s;
    if (!own()) return W5500_ERR_BUSY;
    w5500_eth_settings(&s);
    lent = buffers;
    reserved = true;
    const int e = port_up || port_start(&s) == ESP_OK ? W5500_OK : W5500_ERR_BUS;
    disown();
    return e;
}

void w5500_eth_stop(void)
{
    if (owner != xTaskGetCurrentTaskHandle()) return;   /* only the owner stops the chip */
    if (chip.sock[0].open) w5500_udp_close(&chip, 0);
    if (!reserved) port_stop();
    disown();
}

int w5500_eth_start(w5500_eth_role role)
{
    w5500 *w = &chip;
    w5500_settings s;
    if (!own()) return W5500_ERR_BUSY;
    if (port_up && chip.sock[0].open) w5500_udp_close(w, 0);
    w5500_eth_settings(&s);
    /* A reserved port starts again only after a bus error or for new pins or clock. */
    if (port_up && (chip.bus_errors || !same_port(&s, &port_settings))) port_stop();
    if (!port_up && port_start(&s) != ESP_OK) {
        w5500_eth_stop();
        return W5500_ERR_BUS;
    }
    memset(w, 0, sizeof *w);
    w5500_config c = {0};
    esp_read_mac(c.mac, ESP_MAC_ETH);
    memcpy(c.ip, s.ip, 4);
    memcpy(c.mask, s.mask, 4);
    memcpy(c.gateway, s.gateway, 4);
    if (role == W5500_ETH_SEND) c.tx_kb[0] = 16;
    else c.rx_kb[0] = 16;
    int e = w5500_init(w, &bus, &c);
    if (!e) e = w5500_udp_open(w, 0, s.port);
    if (!e && role == W5500_ETH_SEND) w5500_udp_dest(w, 0, s.dest, s.dest_port);
    if (e) w5500_eth_stop();
    return e;
}

const char *w5500_eth_status(int e)
{
    switch (e) {
    case W5500_OK: return "ok";
    case W5500_FULL: return "full";
    case W5500_ERR_ARG: return "argument";
    case W5500_ERR_BUS: return "spi";
    case W5500_ERR_CHIP: return "chip";
    case W5500_ERR_SOCKET: return "socket";
    case W5500_ERR_BUSY: return "busy: another task owns the chip";
    default: return "?";
    }
}

/* CPU load on this core during a diagnostic, from the idle task: an idle hook keeps it spinning
 * (it returns false, so the core never waits for an interrupt) and adds up the gaps between its
 * calls that are shorter than IDLE_GAP; a longer gap means something else ran. An interrupt or
 * task shorter than IDLE_GAP counts as idle. The load before the diagnostic, with the console
 * task asleep, is measured the same way and reported beside it. */
#define IDLE_GAP (10 * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)   /* 10 us */

static volatile uint32_t idle_last;
static volatile uint64_t idle_cycles;

static bool idle_hook(void)
{
    const uint32_t now = esp_cpu_get_cycle_count();
    const uint32_t gap = now - idle_last;
    idle_last = now;
    if (gap < IDLE_GAP) idle_cycles += gap;
    return false;
}

static bool load_start(void)
{
    idle_cycles = 0;
    idle_last = esp_cpu_get_cycle_count();
    return esp_register_freertos_idle_hook_for_cpu(idle_hook, xPortGetCoreID()) == ESP_OK;
}

/* The idle cycles counted since load_start. The console task outranks the idle task, which may
 * have stopped inside the hook: two ticks of sleep let it finish before the count is read. */
static uint64_t load_stop(void)
{
    esp_deregister_freertos_idle_hook_for_cpu(idle_hook, xPortGetCoreID());
    vTaskDelay(2);
    return idle_cycles;
}

/* The share of us microseconds that this core was not idle. */
static double busy(uint64_t idle, int64_t us)
{
    return us > 0 ? 1.0 - (double)idle / ((double)us * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) : -1;
}

/* The load of this core over ms milliseconds with this task asleep. */
static double baseline(unsigned ms)
{
    if (!load_start()) return -1;
    const int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(ms));
    const int64_t us = esp_timer_get_time() - t0;
    return busy(load_stop(), us);
}

static void print_run(int e, int64_t us, double base, double load, int64_t driver_us,
                      uint32_t calls)
{
    printf(",\"status\":\"%s\",\"spi\":\"%s\",\"us\":%lld,\"driver_us_per_call\":%.1f,"
           "\"polled\":%" PRIu32 ",\"queued\":%" PRIu32 ",\"sleeps\":%" PRIu32 ",\"sck_khz\":%d,"
           "\"heap_bytes\":%u", w5500_eth_status(e), w5500_port.name, (long long)us,
           calls ? (double)driver_us / calls : 0.0, w5500_port.polled, w5500_port.queued,
           w5500_port.sleeps, w5500_port.khz, (unsigned)w5500_port.heap_bytes);
    if (base >= 0 && load >= 0) printf(",\"core_load\":%.4f,\"core_load_before\":%.4f", load, base);
    if (e == W5500_ERR_BUS) printf(",\"esp_err\":\"%s\"", esp_err_to_name(w5500_port.err));
}

/* A diagnostic that could not start reports why and nothing else (the counters may belong to
 * another owner's session). */
static bool started(int e, uint8_t *buf, const char *what)
{
    if (e == W5500_OK) return true;
    printf("@{\"cmd\":\"eth\",\"%s\":true,\"status\":\"%s\"", what, w5500_eth_status(e));
    if (e == W5500_ERR_BUS) printf(",\"esp_err\":\"%s\"", esp_err_to_name(w5500_port.err));
    printf("}\n");
    if (buf != lent) heap_caps_free(buf);
    return false;
}

/* Numbered TS datagrams (w5500_diag_send) from two alternating buffers, since a posted payload
 * may still be clocking out of the last one. */
static uint8_t *buffers(void)
{
    if (lent) return lent;
    return heap_caps_aligned_alloc(4, 2 * DATAGRAM, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

static void diag_send(unsigned ms, double mbps)
{
    uint8_t *pair = buffers();
    uint8_t *const buf[2] = {pair, pair + DATAGRAM};
    int e = pair ? w5500_eth_start(W5500_ETH_SEND) : W5500_ERR_ARG;
    if (!started(e, pair, "send")) return;
    uint32_t seq = 0, calls = 0;
    int64_t driver_us = 0;
    const double base = baseline(20);
    const bool load = load_start();
    const int64_t t0 = esp_timer_get_time();
    while (e >= 0) {
        const int64_t now = esp_timer_get_time();
        if (now - t0 >= ms * 1000LL) break;
        if (mbps > 0 && seq * (8.0 * DATAGRAM) / mbps > now - t0) {   /* ahead of the pace */
            e = w5500_udp_poll(&chip, 0);
            vTaskDelay(1);
            continue;
        }
        const int64_t a = esp_timer_get_time();
        e = w5500_diag_send(&chip, 0, buf, seq);
        driver_us += esp_timer_get_time() - a;
        calls++;
        seq += e == W5500_OK;
    }
    const int64_t us = esp_timer_get_time() - t0;
    const double used = load ? busy(load_stop(), us) : -1;
    for (int i = 0; e >= 0 && i < 100 && (e = w5500_udp_poll(&chip, 0)) > 0; ++i) vTaskDelay(1);
    const w5500_stats *st = &chip.sock[0].st;
    printf("@{\"cmd\":\"eth\",\"send\":true,\"datagrams\":%" PRIu32 ",\"sent\":%" PRIu32
           ",\"full\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"mbps\":%.3f",
           seq, st->sent, st->full, st->dropped, us ? st->sent_bytes * 8.0 / us : 0.0);
    print_run(e > 0 ? W5500_OK : e, us, base, used, driver_us, calls);
    printf("}\n");
    w5500_eth_stop();   /* closes the socket, so no transfer is still reading the buffers */
    if (pair != lent) heap_caps_free(pair);
}

static void diag_recv(unsigned ms)
{
    uint8_t *buf = buffers();      /* W5500_UDP_MAX fits in the pair */
    int e = buf ? w5500_eth_start(W5500_ETH_RECV) : W5500_ERR_ARG;
    if (!started(e, buf, "recv")) return;
    uint32_t sync_errors = 0, not_ts = 0, calls = 0;
    int64_t driver_us = 0;
    w5500_peer peer = {{0}, 0};
    const double base = baseline(20);
    const bool load = load_start();
    const int64_t t0 = esp_timer_get_time();
    while (e >= 0 && esp_timer_get_time() - t0 < ms * 1000LL) {
        const int64_t a = esp_timer_get_time();
        const int n = w5500_udp_recv(&chip, 0, buf, W5500_UDP_MAX, &peer);
        driver_us += esp_timer_get_time() - a;
        calls++;
        if (n <= 0) {
            e = n;
            if (!n) vTaskDelay(1);
            continue;
        }
        if (n % TS_PACKET) not_ts++;
        else for (int k = 0; k < n; k += TS_PACKET) sync_errors += buf[k] != 0x47;
    }
    const int64_t us = esp_timer_get_time() - t0;
    const double used = load ? busy(load_stop(), us) : -1;
    const w5500_stats *st = &chip.sock[0].st;
    printf("@{\"cmd\":\"eth\",\"recv\":true,\"received\":%" PRIu32 ",\"bytes\":%" PRIu32
           ",\"mbps\":%.3f,\"overruns\":%" PRIu32 ",\"discarded\":%" PRIu32 ",\"malformed\":%" PRIu32
           ",\"sync_errors\":%" PRIu32 ",\"not_ts\":%" PRIu32 ",\"peer\":\"%u.%u.%u.%u:%u\"",
           st->received, st->received_bytes, us ? st->received_bytes * 8.0 / us : 0.0,
           st->overruns, st->discarded, st->malformed, sync_errors, not_ts, peer.ip[0], peer.ip[1],
           peer.ip[2], peer.ip[3], peer.port);
    print_run(e, us, base, used, driver_us, calls);
    printf("}\n");
    w5500_eth_stop();
    if (buf != lent) heap_caps_free(buf);
}

static void status(void)
{
    w5500_settings s;
    char json[320];
    uint8_t mac[6];
    w5500_eth_settings(&s);
    w5500_settings_json(&s, json, sizeof json);
    esp_read_mac(mac, ESP_MAC_ETH);
    const int e = w5500_eth_start(W5500_ETH_RECV);
    const int phy = e ? e : w5500_phy(&chip);
    const unsigned version = e == W5500_ERR_BUSY ? 0 : chip.version;
    printf("@{\"cmd\":\"eth\",%s,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"status\":\"%s\","
           "\"version\":%u,\"spi\":\"%s\",\"sck_khz_actual\":%d,\"heap_bytes\":%u", json, mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5], w5500_eth_status(phy < 0 ? phy : W5500_OK),
           version, w5500_port.name, w5500_port.khz, (unsigned)w5500_port.heap_bytes);
    if (phy >= 0)
        printf(",\"link\":%s,\"link_mbps\":%d,\"duplex\":\"%s\",\"phycfgr\":\"0x%02x\"",
               phy & W5500_PHY_LINK ? "true" : "false", phy & W5500_PHY_100M ? 100 : 10,
               phy & W5500_PHY_FULL ? "full" : "half", phy);
    if (e == W5500_ERR_BUS) printf(",\"esp_err\":\"%s\"", esp_err_to_name(w5500_port.err));
    printf("}\n");
    w5500_eth_stop();
}

void w5500_eth_command(const char *args)
{
    unsigned ms;
    double mbps = 0;
    int used = 0;
    char extra;
    while (*args == ' ' || *args == '\t') ++args;
    if (!*args) {
        status();
        return;
    }
    if (sscanf(args, "send %u %n", &ms, &used) == 1) {
        const char *rest = args + used;
        if (*rest && (sscanf(rest, "%lf %c", &mbps, &extra) != 1 || !(mbps > 0 && mbps <= 100)))
            goto usage;
        if (ms < 1 || ms > 600000) goto usage;
        diag_send(ms, mbps);
        return;
    }
    if (sscanf(args, "recv %u %c", &ms, &extra) == 1) {
        if (ms < 1 || ms > 600000) goto usage;
        diag_recv(ms);
        return;
    }
    w5500_settings s;
    w5500_eth_settings(&s);
    if (w5500_settings_parse(&s, args) < 0) goto usage;
    if (!strncmp(args, "defaults", 8)) app_defaults(&s);   /* the parse took nothing else */
    settings = s;
    char json[320];
    w5500_settings_json(&s, json, sizeof json);
#if CONFIG_W5500_SETTINGS_NVS
    const esp_err_t err = save(&s);
    printf("@{\"cmd\":\"eth\",%s,\"saved\":%s", json, err == ESP_OK ? "true" : "false");
    if (err != ESP_OK)
        printf(",\"error\":\"NVS write failed\",\"esp_err\":\"%s\"", esp_err_to_name(err));
#else
    printf("@{\"cmd\":\"eth\",%s,\"saved\":false,\"kept\":\"until reset\"", json);
#endif
    printf("}\n");
    return;
usage:
    printf("@{\"cmd\":\"eth\",\"error\":\"usage: eth; eth ip A.B.C.D/N [gw A.B.C.D]; "
           "eth dest A.B.C.D|broadcast[:PORT]; eth port PORT; eth mhz 10..40; "
           "eth pins SCLK MOSI MISO CS; eth defaults; eth send MS [Mb/s]; eth recv MS\"}\n");
}
