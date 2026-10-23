#pragma once
// machine.c builds these oracles for the host only (machine.h); tests/tx_pie_bench.c defines
// them for the board so the reused host checks can run there.
#include <stdint.h>
void machine_render(uint32_t *dst, const uint32_t *signs, unsigned blocks, const uint32_t *table);
uint32_t machine_expand(uint32_t *dst, const uint32_t *axes, uint32_t history);
