#pragma once
#include "ingress.h"

// Reproducible sender/check pattern. TS: PID 0x100, sequence and packet index in bytes 4..7.
// GCS: the same 7*188 bytes with no TS header. Pattern is a test source, not a framing rule.
void ingress_pattern(uint8_t *payload, uint16_t seq, unsigned type);
// PigeonVision's public test pattern: PID 0x100, 112-packet period.
void ingress_pv_pattern(uint8_t *payload, uint16_t seq);
