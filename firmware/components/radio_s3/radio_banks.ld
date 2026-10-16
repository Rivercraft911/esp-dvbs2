/* Runtime heap exclusion alone cannot protect these banks from static data or
 * the instruction-bus aliases. Fail the link if either static allocation grows
 * into them. The addresses and +0x6f0000 alias are documented in ESP-IDF v6.1
 * components/heap/port/esp32s3/memory_layout.c:90-97.
 */
ASSERT(_heap_start <= 0x3fcc0000, "radio DAC banks overlap static DRAM")
ASSERT(_iram_end <= 0x403b0000, "radio DAC banks overlap static IRAM alias")
