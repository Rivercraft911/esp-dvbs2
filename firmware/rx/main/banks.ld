/* Three capture banks at 0x3fcb0000..0x3fce0000, and their instruction-bus aliases. */
ASSERT(_heap_start <= 0x3fcb0000, "RX banks overlap static DRAM")
ASSERT(_iram_end <= 0x403a0000, "RX banks overlap static IRAM alias")
