// Actual cache residency lifecycle, with ROM operations replaced. No board.
#include <assert.h>
#include <stdint.h>
#define LOFTY_CACHE_HOST
#define _lofty_text_start ((const uint8_t *)0x42001000)
#define _lofty_text_end ((const uint8_t *)0x42001841)
static unsigned clock_now, calls, loading, locked, ended;
static int timeout, lock_error, unlock_error;
static uint32_t esp_cpu_get_cycle_count(void) { return clock_now += 100000; }
static uint32_t Cache_Start_ICache_Preload(uint32_t first, uint32_t bytes, uint32_t order)
{
    assert(first == 0x42001000 && bytes == 2144 && !order && !loading);
    ++calls; loading = 1; return 3;
}
static int Cache_ICache_Preload_Done(void) { return !timeout; }
static void Cache_End_ICache_Preload(uint32_t autoload)
{ assert(autoload == 3 && loading); loading = 0; ++ended; }
static int Cache_Lock_Addr(uint32_t first, uint32_t bytes)
{
    assert(first == 0x42001000 && bytes == 2144 && !loading);
    if (lock_error) return 1;
    locked = 1; return 0;
}
static int Cache_Unlock_Addr(uint32_t first, uint32_t bytes)
{
    assert(first == 0x42001000 && bytes == 2144 && locked);
    if (unlock_error) return 1;
    locked = 0; return 0;
}
#include "../firmware/main/lofty.c"
int main(void)
{
    assert(!lofty_resident(0) && !calls && !resident && !lofty_resident_bytes());
    timeout = 1;
    assert(lofty_resident(1) && !resident && !locked && !loading && ended == 1);
    assert(!lofty_resident(0));
    timeout = 0; lock_error = 1;
    assert(lofty_resident(1) && !resident && !locked && !loading && ended == 2);
    lock_error = 0;
    for (unsigned i = 0; i < 3; ++i) {
        assert(!lofty_resident(1) && locked && resident && lofty_resident_bytes() == 2144);
        unsigned before = calls;
        assert(!lofty_resident(1) && calls == before); // repeated setup
        unlock_error = 1;
        assert(lofty_resident(0) && locked && resident);
        unlock_error = 0;
        assert(!lofty_resident(0) && !locked && !resident && !lofty_resident_bytes());
        assert(!lofty_resident(0)); // repeated/failure cleanup
    }
    return 0;
}
