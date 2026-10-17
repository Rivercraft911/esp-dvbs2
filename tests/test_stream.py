"""Run real stream startup, encoder, ring, fill and dump on the host. No board access.

The plain encoder checks every DAC word. Test two-slot wraps, notifications, repeated
commands, allocation failure cleanup and dump CRCs. All clocks/heap sizes are synthetic.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
NATIVE = r'''
#pragma once
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define IRAM_ATTR
#define DRAM_ATTR
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
typedef int esp_err_t;
static inline const char *esp_err_to_name(int e) { return e ? "ESP_FAIL" : "ESP_OK"; }
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
typedef uint32_t TickType_t;
static inline TickType_t xTaskGetTickCount(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static inline uint32_t esp_cpu_get_cycle_count(void) { return 17; }
static int fail_after = -1, live_allocations, allocation_count;
static struct { void *p; size_t n; unsigned caps; } native_blocks[64];
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, unsigned caps) {
    ++allocation_count;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *p = NULL; assert(!posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n));
    for (unsigned i = 0; i < 64; ++i)
        if (!native_blocks[i].p) { native_blocks[i].p = p; native_blocks[i].n = n;
                                   native_blocks[i].caps = caps; break; }
    ++live_allocations; return p;
}
static inline void *heap_caps_malloc(size_t n, unsigned caps) {
    return heap_caps_aligned_alloc(16, n, caps);
}
static inline size_t heap_caps_get_allocated_size(void *p) {
    for (unsigned i = 0; i < 64; ++i) if (native_blocks[i].p == p) return native_blocks[i].n;
    abort();
}
static inline void heap_caps_free(void *p) {
    if (!p) return;
    for (unsigned i = 0; i < 64; ++i) if (native_blocks[i].p == p) native_blocks[i].p = NULL;
    --live_allocations; free(p);
}
static inline size_t heap_caps_get_free_size(unsigned c) { (void)c; return 65536; }
#define heap_caps_get_largest_free_block heap_caps_get_free_size
#define heap_caps_get_minimum_free_size heap_caps_get_free_size
static inline int heap_caps_monitor_local_minimum_free_size_start(void) { return 0; }
static inline int heap_caps_monitor_local_minimum_free_size_stop(void) { return 0; }
typedef struct {
    pthread_t thread; pthread_mutex_t mutex; pthread_cond_t cond;
    unsigned notifications; void (*fn)(void *); void *arg;
} native_task;
typedef native_task *TaskHandle_t;
static _Thread_local TaskHandle_t current;
static TaskHandle_t worker;
static inline void vTaskDelay(unsigned ticks) {
    struct timespec t = {ticks / 1000, (long)(ticks % 1000) * 1000000}; nanosleep(&t, NULL);
}
static inline void xTaskNotifyGive(TaskHandle_t t) {
    pthread_mutex_lock(&t->mutex); ++t->notifications;
    pthread_cond_signal(&t->cond); pthread_mutex_unlock(&t->mutex);
}
static inline unsigned ulTaskNotifyTake(int clear, unsigned timeout) {
    (void)clear; TaskHandle_t t = current;
    pthread_mutex_lock(&t->mutex);
    if (timeout == portMAX_DELAY) {
        while (!t->notifications) pthread_cond_wait(&t->cond, &t->mutex);
    } else if (!t->notifications) {   // one tick is 1 ms (CONFIG_FREERTOS_HZ=1000)
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)timeout * 1000000;
        until.tv_sec += until.tv_nsec / 1000000000; until.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&t->cond, &t->mutex, &until);
    }
    unsigned n = t->notifications; t->notifications = 0;
    pthread_mutex_unlock(&t->mutex); return n;
}
static inline void vTaskSuspend(TaskHandle_t t) { (void)t; pthread_exit(NULL); }
static inline void vTaskDelete(TaskHandle_t t) {
    assert(t == worker); pthread_join(t->thread, NULL);
    pthread_mutex_destroy(&t->mutex); pthread_cond_destroy(&t->cond); free(t);
}
static void *native_entry(void *arg) {
    current = arg; current->fn(current->arg); return NULL;
}
static inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle, int core) {
    (void)name; (void)stack; (void)priority; (void)core;
    worker = calloc(1, sizeof *worker); worker->fn = fn; worker->arg = arg;
    pthread_mutex_init(&worker->mutex, NULL); pthread_cond_init(&worker->cond, NULL);
    *handle = worker; assert(!pthread_create(&worker->thread, NULL, native_entry, worker));
    return pdPASS;
}
typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; unsigned ready; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {
    static int initialized;
    if (!initialized) { pthread_mutex_init(&s->mutex, NULL); pthread_cond_init(&s->cond, NULL); initialized = 1; }
    s->ready = 0; return s;
}
static inline void xSemaphoreGive(SemaphoreHandle_t s) {
    pthread_mutex_lock(&s->mutex); s->ready = 1;
    pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->mutex);
}
static inline void xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) {
    (void)timeout; pthread_mutex_lock(&s->mutex);
    while (!s->ready) pthread_cond_wait(&s->cond, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
}
#define EXTMEM_IBUS_TO_FLASH_START_VADDR_REG 0
#define EXTMEM_IBUS_TO_FLASH_END_VADDR_REG 1
#define EXTMEM_DBUS_TO_FLASH_START_VADDR_REG 2
#define EXTMEM_DBUS_TO_FLASH_END_VADDR_REG 3
static uint32_t native_ranges[4];
#define REG_READ(r) native_ranges[r]
#define REG_WRITE(r,v) (native_ranges[r] = (v))
static inline uint32_t esp_rom_crc32_le(uint32_t c, const uint8_t *b, size_t n) {
    c = ~c; while (n--) { c ^= *b++; for (unsigned k = 0; k < 8; ++k)
        c = c >> 1 ^ (0xedb88320u & (0u - (c & 1))); } return ~c;
}
int usb_serial_jtag_write_bytes(const void *, size_t, unsigned);
'''


def test_stream(output=None):
    if output:
        output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        (folder / "native.h").write_text(NATIVE)
        for header in ("esp_err.h", "esp_cpu.h", "esp_attr.h", "esp_heap_caps.h", "esp_rom_crc.h",
                       "driver/usb_serial_jtag.h", "freertos/FreeRTOS.h", "freertos/semphr.h",
                       "freertos/task.h"):
            path = folder / header
            path.parent.mkdir(exist_ok=True)
            path.write_text('#include "native.h"\n')
        exe = folder / "stream"
        subprocess.run(["cc", "-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                        "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-I" + tmp, "-I" + str(ROOT / "firmware/main"),
                        "-I" + str(ROOT / "firmware/components/dvbs2/include"),
                        "-I" + str(ROOT / "firmware/components/radio_s3/include"),
                        "-I" + str(ROOT / "firmware/components/station_id/include"),
                        str(ROOT / "tests/stream_host.c"), str(ROOT / "firmware/main/ring.c"),
                        str(ROOT / "firmware/main/shaper.c"),
                        str(ROOT / "firmware/main/shaper_short.c"),
                        str(ROOT / "firmware/components/dvbs2/dvbs2.c"),
                        str(ROOT / "firmware/components/dvbs2/fast.c"),
                        str(ROOT / "firmware/components/station_id/station_id.c"),
                        "-lm", "-o", str(exe)], check=True)
        crcs = {}
        for mc, sps, call, rate, short in ((13, 4, "N0CALL", 40, 0), (15, 4, "N0CALL", 40, 0), (17, 4, "", 40, 0),
                                    (12, 8, "", 40, 0), (13, 8, "N0CALL", 40, 0), (15, 8, "N0CALL", 40, 0),
                                    (17, 8, "", 40, 0), (4, 40, "N0CALL", 40, 0), (13, 4, "N0CALL", 80, 0),
                                    (15, 4, "N0CALL", 80, 0), (17, 4, "", 80, 0), (12, 8, "", 80, 0),
                                    (13, 8, "N0CALL", 80, 0), (13, 4, "N0CALL", 80, 1),
                                    (15, 4, "N0CALL", 80, 1), (17, 4, "", 80, 1)):
            iq = output / f'{mc}-n{sps}-{rate}m-s{6 if short else 9}.iq' if output else folder / "dump.iq"
            out = subprocess.run([str(exe), str(mc), str(sps), str(iq), call, str(rate * 1000000), str(short)],
                                 capture_output=True, text=True, timeout=60, check=True).stdout
            records = [json.loads(line[1:]) for line in out.splitlines() if line.startswith("@")]
            dumps = [r for r in records if "crc32" in r]
            assert len(dumps) == 2
            # The five published 40-frame board CRCs, with N0CALL in the payload header.
            expected = {(4, 40): 1435641601, (13, 8): 1937985440, (15, 8): 1163322913,
                        (13, 4): 474263962, (15, 4): 1757765864}
            for r in dumps:
                assert r["crc32"] == zlib.crc32(iq.read_bytes())
                if not short and (mc, sps) in expected:
                    assert r["crc32"] == expected[mc, sps]
                assert r["status"] == "ESP_OK" and not r["encoder_failed"]
                assert r["callsign"] == call
                packed = sps != 40
                profile = r["profile"]
                assert profile["shaped_words"] + profile["reused_words"] == profile["fill_words"]
                assert profile["reused_words"] > 0 if packed else profile["reused_words"] == 0
                assert r["ring_slots"] == (3 if sps == 4 else 2 if packed else 8)
                assert r["prefill_frames"] == r["ring_slots"]
                assert 0 <= r["encode_cycles_live_max"] <= r["encode_cycles_max"]
                assert r["ring_memory"] == ("internal" if packed else "psram")
                assert r["ring_format"] == ("packed" if packed else "raw")
                assert r["workspace_memory"] == ("internal" if sps == 4 else "psram")
                assert r["workspace_bytes"] == 12420
                assert r["workspace_heap_bytes"] == r["workspace_bytes"]
                assert r["fast_heap_bytes"] == r["fast_bytes"]
                cached = sps == 4 and mc != 17
                assert r["bch_cache_bytes"] == (8192 if cached else 0)
                assert r["bch_cache_heap_bytes"] == r["bch_cache_bytes"]
                assert r["bch_variant"] == ("resident-byte" if cached else "nibble")
                assert bool(int(r["layout"]["bch"], 16)) == cached
                slots = [int(a, 16) for a in r["layout"]["slots"]]
                assert len(set(slots)) == r["ring_slots"]
                payload = int(r["layout"]["payload"], 16)
                filler = int(r["layout"]["filler"], 16)
                if packed:
                    # Two packed frames, the packed dummy, then the payload, each with the
                    # 16 bytes the vector unpack may read past its end.
                    frame = (22194 // 2 + 31) // 16 * 16
                    assert slots[:2] == [slots[0], slots[0] + frame]
                    assert all(a % 16 == 0 for a in slots)
                    assert filler == slots[0] + 2 * frame
                    assert payload == filler + (3330 // 2 + 31) // 16 * 16
                    need = payload - slots[0] + r["payload_bytes_per_frame"]
                    if short:
                        assert slots[0] == int(r["layout"]["table"], 16) + 16384
                        assert slots[2] >= payload + r["payload_bytes_per_frame"]
                        assert slots[2] + frame <= int(r["layout"]["table"], 16) + 65536
                        assert r["ring_heap_bytes"] == 0
                    elif sps == 4:
                        # Two frames, dummy and payload in the store; the third on the heap.
                        assert slots[0] == int(r["layout"]["table"], 16) + 32768
                        assert need <= 32768 and r["ring_heap_bytes"] == frame
                        assert not slots[0] <= slots[2] < slots[0] + 32768
                    else:
                        assert r["ring_heap_bytes"] == need
                assert all(p["calls"] == r["frames_encoded"] for p in r["profile"]["encoder"].values())
                assert r["symbol_rate"] == rate * 1000000 // sps
            # The fill depends on the sample index only: the 80 MS/s bank geometry (other lead,
            # tail and step) must give the same samples as 40 MS/s.
            crcs[mc, sps, call, rate, short] = dumps[0]["crc32"]
            if rate == 80 and not short:
                assert crcs[mc, sps, call, 80, 0] == crcs[mc, sps, call, 40, 0]
            print(f"stream host: MODCOD {mc}, {sps} sps at {rate} MS/s, {dumps[0]['ring_format']} ring, "
                  f"callsign {call or 'none'}, 2 x 40 frames, "
                  f"CRC {dumps[0]['crc32']}, "
                  "all DAC words match plain encoder; cleanup and notifications pass")

        # More than one complete physical-wrap cycle for both geometries. Every
        # word is still checked by the plain encoder oracle inside the C harness.
        for rate, short, frames in ((40000000, 0, 256), (80000000, 1, 160)):
            subprocess.run([str(exe), "13", "4", str(folder / "wrap.iq"), "N0CALL",
                            str(rate), str(short), str(frames)], capture_output=True,
                           text=True, timeout=120, check=True)
        print("stream host: complete 1365/819-bank wrap cycles, sample reuse matches the oracle")

if __name__ == "__main__":
    test_stream(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else None)
