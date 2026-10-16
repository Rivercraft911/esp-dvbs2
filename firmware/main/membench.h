#pragma once

// Time the memory work a transmitter must do: filling one DAC bank, and moving data to and from
// PSRAM. Prints one '@' JSON line per test.
void membench_run(void);
