// Actual cache residency lifecycle, with ROM operations replaced. No board.
#include <assert.h>
#include <stdint.h>
#define LOFTY_CACHE_HOST
#define _lofty_text_start ((const uint8_t *)0x42001000)
#define _lofty_text_end ((const uint8_t *)0x42001841)
static unsigned clock_now, calls, loading, locked, ended;
static unsigned data_loading, data_locked, data_ended;
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
static uint32_t Cache_Start_DCache_Preload(uint32_t first, uint32_t bytes, uint32_t order)
{
    assert(first == 0x3c001000 && bytes == 13376 && !order && !data_loading && !data_locked);
    data_loading = 1; return 7;
}
static int Cache_DCache_Preload_Done(void) { return !timeout; }
static void Cache_End_DCache_Preload(uint32_t autoload)
{ assert(autoload == 7 && data_loading); data_loading = 0; ++data_ended; }
static int Cache_Lock_Addr(uint32_t first, uint32_t bytes)
{
    if (first == 0x3c001000) {
        assert(bytes == 13376 && !data_loading && !data_locked);
        if (lock_error) return 1;
        data_locked = 1; return 0;
    }
    assert(first == 0x42001000 && bytes == 2144 && !loading);
    if (lock_error) return 1;
    locked = 1; return 0;
}
static int Cache_Unlock_Addr(uint32_t first, uint32_t bytes)
{
    if (first == 0x3c001000) {
        assert(bytes == 13376 && data_locked);
        if (unlock_error) return 1;
        data_locked = 0; return 0;
    }
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
    const uint32_t *dummy = (const uint32_t *)0x3c001000;
    assert(lofty_dummy_resident(dummy+1,13376,1));
    assert(lofty_dummy_resident(dummy,13375,1));
    assert(lofty_dummy_resident(dummy,16416,1));
    assert(lofty_dummy_resident((const uint32_t *)0x42001000,13376,1));
    assert(lofty_dummy_resident(dummy,0,1));
    timeout = 1;
    assert(lofty_dummy_resident(dummy,13376,1) && !data_locked && !data_loading && data_ended == 1);
    timeout = 0; lock_error = 1;
    assert(lofty_dummy_resident(dummy,13376,1) && !data_locked && !data_loading && data_ended == 2);
    lock_error = 0;
    for (unsigned i=0; i<3; ++i) {
        assert(!lofty_resident(1) && locked);
        assert(!lofty_dummy_resident(dummy,13376,1) && data_locked && locked);
        unlock_error = 1;
        assert(lofty_dummy_resident(dummy,13376,0) && data_locked && locked);
        unlock_error = 0;
        assert(!lofty_dummy_resident(dummy,13376,0) && !data_locked && locked);
        assert(!lofty_resident(0) && !locked);
    }
    return 0;
}
