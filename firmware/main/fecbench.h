#pragma once

// Encode seeded normal frames in every supported mode with one implementation ("plain", "c" or
// "pie"). Reports raw per-stage cycles and an IEEE CRC32 of the output phase bytes, one '@' JSON
// line per timed frame. Does not start RF. Returns -1 for an unknown implementation.
int fecbench_run(const char *impl);
