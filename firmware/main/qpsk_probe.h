#pragma once

// Isolated, radio-off fill experiment. qpsk3/qpsk4 <seconds or frames> [MODCOD 4..11].
// The encoder runs BEFORE the timing/dump. This cannot qualify a live encoder or RF.
void qpsk_probe_command(const char *cmd, char *args);
