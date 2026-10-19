#ifndef POLY_H
#define POLY_H
#include <stdint.h>
void poly_pack(uint32_t *, const uint32_t *, unsigned bits, unsigned spacing);
void poly_extract(uint32_t *, const uint32_t *, unsigned bits, unsigned spacing);
void poly_reference(uint32_t *, const uint32_t *, const uint32_t *, unsigned bits);
#endif
