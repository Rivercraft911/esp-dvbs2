#pragma once

#include "radio_s3.h"

/* Private to the exclusive radio worker. The BT slot is a bit, not a counter. */
typedef struct { bool held; } physolo_t;
esp_err_t physolo_available(void);
esp_err_t physolo_acquire(physolo_t *solo);
void physolo_release(physolo_t *solo);
esp_err_t physolo_snapshot(radio_s3_session_stats_t *stats);
/* Current core only: no IPC interrupt of the streaming core. */
void physolo_snapshot_local(radio_s3_session_stats_t *stats);
