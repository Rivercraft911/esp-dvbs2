/* Three capture banks, including their instruction-bus aliases. No bank 3. */
ASSERT(_heap_start <= 0x3fcb0000, "RX banks overlap static DRAM")
ASSERT(_iram_end <= 0x403a0000, "RX banks overlap static IRAM alias")
