#pragma once
#include "ingress.h"

// Reproducible sender/check pattern. TS: PID 0x100, sequence and packet index in bytes 4..7.
// GCS: the same 7*188 bytes with no TS header. Pattern is a test source, not a framing rule.
void ingress_pattern(uint8_t *payload, uint16_t seq, unsigned type);
// Packet j (0..6) of that message alone, 188 bytes.
void ingress_pattern_packet(uint8_t *packet, uint16_t seq, unsigned j, unsigned type);
// Nonzero unless a 188-byte packet is TS packet j (0..6) of message seq; builds nothing.
uint32_t ingress_pattern_ts_differs(const uint8_t *packet, uint16_t seq, unsigned j);
// PigeonVision's public test pattern: PID 0x100, 112-packet period.
void ingress_pv_pattern(uint8_t *payload, uint16_t seq);
