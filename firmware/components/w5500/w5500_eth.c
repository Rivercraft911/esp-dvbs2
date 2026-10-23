/* ESP-IDF side of the W5500 driver: ownership, MAC, NVS settings, `eth` (w5500_eth.h). The SPI
 * is in w5500_spi_idf.c or w5500_spi_direct.c (w5500_port.h). */
#include "w5500_eth.h"
#include "w5500_port.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TS_PACKET 188
#define DATAGRAM (7 * TS_PACKET)
#define STORE_VERSION 1

static w5500 chip;
static bool running;

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

w5500 *w5500_eth(void)
{
    return &chip;
}

esp_err_t w5500_eth_error(void)
{
    return w5500_port.err;
}

typedef struct {
    uint8_t version;
    w5500_settings s;
} stored_settings;

/* Open the settings namespace, mounting NVS only for the moment if the app has not (the
 * receiver does not use NVS and is short of internal RAM). */
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

void w5500_eth_settings(w5500_settings *s)
{
    stored_settings stored;
    size_t size = sizeof stored;
    nvs_handle_t h;
    bool mounted;
    w5500_settings_default(s);
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

static void release(void)
{
    if (!running) return;
    if (chip.sock[0].open) w5500_udp_close(&chip, 0);
    w5500_port_stop();
    running = false;
}

void w5500_eth_stop(void)
{
    if (owner != xTaskGetCurrentTaskHandle()) return;   /* only the owner stops the chip */
    release();
    owner = NULL;
    xSemaphoreGive(owner_lock);
}

int w5500_eth_start(w5500_eth_role role)
{
    w5500 *w = &chip;
    w5500_settings s;
    if (!own()) return W5500_ERR_BUSY;
    release();
    w5500_eth_settings(&s);
    memset(w, 0, sizeof *w);
    w5500_bus b;
    if (w5500_port_start(&s, &b) != ESP_OK) {
        w5500_eth_stop();
        return W5500_ERR_BUS;
    }
    running = true;
    w5500_config c = {0};
    esp_read_mac(c.mac, ESP_MAC_ETH);
    memcpy(c.ip, s.ip, 4);
    memcpy(c.mask, s.mask, 4);
    memcpy(c.gateway, s.gateway, 4);
    if (role == W5500_ETH_SEND) c.tx_kb[0] = 16;
    else c.rx_kb[0] = 16;
    int e = w5500_init(w, &b, &c);
    if (!e) e = w5500_udp_open(w, 0, s.port);
    if (!e && role == W5500_ETH_SEND) w5500_udp_dest(w, 0, s.dest, s.dest_port);
    if (e) w5500_eth_stop();
    return e;
}

static const char *name(int e)
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

/* Idle time of this core in microseconds, when FreeRTOS keeps run-time statistics in esp_timer
 * units (the TX app does); otherwise the load is not reported. */
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#define LOAD 1
static uint64_t idle_us(void)
{
    return ulTaskGetIdleRunTimeCounterForCore(xPortGetCoreID());
}
#else
#define LOAD 0
static uint64_t idle_us(void)
{
    return 0;
}
#endif

static void print_run(int e, int64_t us, uint64_t idle, int64_t driver_us, uint32_t calls)
{
    printf(",\"status\":\"%s\",\"spi\":\"%s\",\"us\":%lld,\"driver_us_per_call\":%.1f,"
           "\"polled\":%" PRIu32 ",\"queued\":%" PRIu32 ",\"sleeps\":%" PRIu32 ",\"sck_khz\":%d,"
           "\"heap_bytes\":%u", name(e), w5500_port.name, (long long)us,
           calls ? (double)driver_us / calls : 0.0, w5500_port.polled, w5500_port.queued,
           w5500_port.sleeps, w5500_port.khz, (unsigned)w5500_port.heap_bytes);
    if (LOAD && us) printf(",\"core_load\":%.3f", 1.0 - (double)idle / (double)us);
    if (e == W5500_ERR_BUS) printf(",\"esp_err\":\"%s\"", esp_err_to_name(w5500_port.err));
}

/* A diagnostic that could not start reports why and nothing else (the counters may belong to
 * another owner's session). */
static bool started(int e, uint8_t *buf, const char *what)
{
    if (e == W5500_OK) return true;
    printf("@{\"cmd\":\"eth\",\"%s\":true,\"status\":\"%s\"", what, name(e));
    if (e == W5500_ERR_BUS) printf(",\"esp_err\":\"%s\"", esp_err_to_name(w5500_port.err));
    printf("}\n");
    heap_caps_free(buf);
    return false;
}

/* Numbered TS datagrams (w5500_diag_send) from two alternating buffers, since a posted payload
 * may still be clocking out of the last one. */
static void diag_send(unsigned ms, double mbps)
{
    uint8_t *pair = heap_caps_aligned_alloc(4, 2 * DATAGRAM, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *const buf[2] = {pair, pair + DATAGRAM};
    int e = pair ? w5500_eth_start(W5500_ETH_SEND) : W5500_ERR_ARG;
    if (!started(e, pair, "send")) return;
    uint32_t seq = 0, calls = 0;
    int64_t driver_us = 0;
    const int64_t t0 = esp_timer_get_time();
    const uint64_t idle0 = idle_us();
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
    const uint64_t idle = idle_us() - idle0;
    for (int i = 0; e >= 0 && i < 100 && (e = w5500_udp_poll(&chip, 0)) > 0; ++i) vTaskDelay(1);
    const w5500_stats *st = &chip.sock[0].st;
    printf("@{\"cmd\":\"eth\",\"send\":true,\"datagrams\":%" PRIu32 ",\"sent\":%" PRIu32
           ",\"full\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"mbps\":%.3f",
           seq, st->sent, st->full, st->dropped, us ? st->sent_bytes * 8.0 / us : 0.0);
    print_run(e > 0 ? W5500_OK : e, us, idle, driver_us, calls);
    printf("}\n");
    w5500_eth_stop();   /* closes the socket, so no transfer is still reading the buffers */
    heap_caps_free(pair);
}

static void diag_recv(unsigned ms)
{
    uint8_t *buf = heap_caps_aligned_alloc(4, W5500_UDP_MAX, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    int e = buf ? w5500_eth_start(W5500_ETH_RECV) : W5500_ERR_ARG;
    if (!started(e, buf, "recv")) return;
    uint32_t sync_errors = 0, not_ts = 0, calls = 0;
    int64_t driver_us = 0;
    w5500_peer peer = {{0}, 0};
    const int64_t t0 = esp_timer_get_time();
    const uint64_t idle0 = idle_us();
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
    const uint64_t idle = idle_us() - idle0;
    const w5500_stats *st = &chip.sock[0].st;
    printf("@{\"cmd\":\"eth\",\"recv\":true,\"received\":%" PRIu32 ",\"bytes\":%" PRIu32
           ",\"mbps\":%.3f,\"overruns\":%" PRIu32 ",\"discarded\":%" PRIu32 ",\"malformed\":%" PRIu32
           ",\"sync_errors\":%" PRIu32 ",\"not_ts\":%" PRIu32 ",\"peer\":\"%u.%u.%u.%u:%u\"",
           st->received, st->received_bytes, us ? st->received_bytes * 8.0 / us : 0.0,
           st->overruns, st->discarded, st->malformed, sync_errors, not_ts, peer.ip[0], peer.ip[1],
           peer.ip[2], peer.ip[3], peer.port);
    print_run(e, us, idle, driver_us, calls);
    printf("}\n");
    w5500_eth_stop();
    heap_caps_free(buf);
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
           mac[1], mac[2], mac[3], mac[4], mac[5], name(phy < 0 ? phy : W5500_OK), version,
           w5500_port.name, w5500_port.khz, (unsigned)w5500_port.heap_bytes);
    if (phy >= 0)
        printf(",\"link\":%s,\"link_mbps\":%d,\"duplex\":\"%s\"",
               phy & W5500_PHY_LINK ? "true" : "false", phy & W5500_PHY_100M ? 100 : 10,
               phy & W5500_PHY_FULL ? "full" : "half");
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
    const esp_err_t err = save(&s);
    char json[320];
    w5500_settings_json(&s, json, sizeof json);
    printf("@{\"cmd\":\"eth\",%s,\"saved\":%s", json, err == ESP_OK ? "true" : "false");
    if (err != ESP_OK)
        printf(",\"error\":\"NVS write failed\",\"esp_err\":\"%s\"", esp_err_to_name(err));
    printf("}\n");
    return;
usage:
    printf("@{\"cmd\":\"eth\",\"error\":\"usage: eth; eth ip A.B.C.D/N [gw A.B.C.D]; "
           "eth dest A.B.C.D|broadcast[:PORT]; eth port PORT; eth mhz 10..40; "
           "eth pins SCLK MOSI MISO CS; eth defaults; eth send MS [Mb/s]; eth recv MS\"}\n");
}
