#pragma once

// Encode seeded normal frames in every supported mode. Reports raw per-stage cycles and an
// IEEE CRC32 of the output phase bytes, one '@' JSON line per timed frame. Does not start RF.
void fecbench_run(void);
