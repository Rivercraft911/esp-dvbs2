#pragma once

// Paced diagnostics only. Call enable before starting either worker.
void stream_jitter_enable(int enabled);
void stream_jitter_core_start(void);
void stream_jitter_core_stop(void);
void stream_jitter_print(void);
