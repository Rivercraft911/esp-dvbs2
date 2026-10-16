// Host model for firmware/main/ring.c, run by tests/test_ring.py.
//
// The fill side calls ring_window in the order of the radio_s3 bank schedule: bank 0 holds
// samples [0, BANK), bank k >= 1 holds [k * STEP - LEAD, ... + BANK), each split at the bank wrap,
// 40 samples per symbol and 8 history symbols as in dvbtx.c. The encoder side writes frame e as
// bytes that identify it, after first filling the slot with POISON. Checks:
//   - every symbol index reads the same byte every time it is read (the bank overlap),
//   - no read sees POISON (a slot being written) and history before symbol 0 reads 0,
//   - the positions hold frames 0, 1, 2, ... in order, with the filler exactly where an underrun
//     was counted.
//
//   ring_host sim <L> <slots> <positions> <encode permille> <burst every> <burst permille> <prime>
//     time in samples; frame e takes permille/1000 of a frame period to encode, every burst-th
//     frame burst permille; the encoder looks for a free slot at every fill.
//   ring_host threads <L> <slots> <positions> <seed>
//     encoder and fill on two threads with random pauses (weak memory order on arm64).

#define _POSIX_C_SOURCE 200809L   // rand_r, nanosleep under -std=c11 on glibc

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ring.h"

#define BANK 16380
#define LEAD 64
#define TAIL 2048
#define STEP (BANK - LEAD - TAIL)
#define SPS 40
#define HISTORY 8
#define WINDOW (HISTORY + 412)
#define POISON 0xEE
#define FILLER 0xFA
#define UNSEEN 0xFFFF

static ring_t ring;
static uint32_t length, nslots;
static uint16_t *seen;
static uint64_t seen_symbols;

static uint8_t frame_byte(uint32_t e, uint32_t i)
{
    uint32_t h = e * 2654435761u ^ (i * 40503u + 0x9e3779b9u);
    h ^= h >> 15, h *= 0x2c1b3c6du, h ^= h >> 12;
    return (uint8_t)(1 + h % 200);   // never POISON, FILLER or 0
}

static void fail(const char *what, long long a, long long b)
{
    printf("FAIL %s %lld %lld\n", what, a, b);
    exit(1);
}

// One fill call, as dvbtx.c makes it, then the overlap and content checks.
static void fill(uint64_t first, size_t count)
{
    uint8_t win[WINDOW];
    const int nsym = ring_window(&ring, win, sizeof win, first, count, SPS, HISTORY, BANK);
    if (nsym < 0) {
        fail("ring_window", (long long)first, (long long)count);
    }
    const int64_t s0 = (int64_t)(first / SPS) - HISTORY;
    for (int j = 0; j < HISTORY + nsym; ++j) {
        const int64_t i = s0 + j;
        if (i < 0) {
            if (win[j]) {
                fail("history before symbol 0", i, win[j]);
            }
            continue;
        }
        if (win[j] == POISON) {
            fail("read a slot being written", i, ring.next_position);
        }
        if ((uint64_t)i >= seen_symbols) {
            fail("past the end", i, (long long)seen_symbols);
        }
        if (seen[i] == UNSEEN) {
            seen[i] = win[j];
        } else if (seen[i] != win[j]) {
            fail("symbol changed between reads", i, win[j]);
        }
    }
}

static uint64_t bank_start(uint64_t k)
{
    return k ? k * STEP - LEAD : 0;
}

static void fill_bank(uint64_t k)
{
    const uint64_t start = bank_start(k);
    const size_t offset = start % BANK;
    fill(start, BANK - offset);
    if (offset) {
        fill(start + BANK - offset, offset);
    }
}

// Banks needed so the fills reach into `positions` positions.
static uint64_t banks_for(uint32_t positions)
{
    return ((uint64_t)positions * length * SPS - BANK) / STEP;
}

static void write_frame(uint8_t *slot, uint32_t e)
{
    for (uint32_t i = 0; i < length; ++i) {
        slot[i] = frame_byte(e, i);
    }
}

// Walk the chosen positions: frames in order, fillers exactly where underruns were counted.
static void check(void)
{
    uint32_t e = 0, fillers = 0;
    for (uint32_t q = 0; q < ring.next_position; ++q) {
        int kind = -1;   // 0 frame, 1 filler
        for (uint32_t i = 0; i < length; ++i) {
            const uint16_t v = seen[(uint64_t)q * length + i];
            if (v == UNSEEN) {
                continue;
            }
            if (kind < 0) {
                kind = v == FILLER;
            }
            if (v != (kind ? FILLER : frame_byte(e, i))) {
                fail(kind ? "filler content" : "frame out of order", q, i);
            }
        }
        if (kind < 0) {
            fail("chosen position never read", q, 0);
        }
        kind ? ++fillers : ++e;
    }
    if (e != ring.next_frame || fillers != ring.underruns) {
        fail("frame or underrun count", e, fillers);
    }
    printf("ok positions %u frames %u underruns %u min_lead %lld\n", ring.next_position, e,
           ring.underruns, (long long)ring.min_lead);
}

static void setup(uint32_t l, uint32_t s, uint32_t positions)
{
    length = l, nslots = s;
    uint8_t *slots = malloc((size_t)s * l), *filler = malloc(l);
    memset(slots, POISON, (size_t)s * l);
    memset(filler, FILLER, l);
    ring_init(&ring, slots, s, filler, l);
    seen_symbols = (uint64_t)(positions + 2) * l;
    seen = malloc(seen_symbols * sizeof *seen);
    for (uint64_t i = 0; i < seen_symbols; ++i) {
        seen[i] = UNSEEN;
    }
}

// Discrete-time model: the encoder works between fills, polling for a slot at each fill.
static int sim(int argc, char **argv)
{
    if (argc != 9) {
        return 2;
    }
    const uint32_t positions = (uint32_t)atoi(argv[4]);
    const uint64_t period = (uint64_t)atoi(argv[2]) * SPS;   // one frame, in samples
    const uint64_t base = period * (uint64_t)atoi(argv[5]) / 1000;
    const uint32_t every = (uint32_t)atoi(argv[6]);
    const uint64_t burst = period * (uint64_t)atoi(argv[7]) / 1000;
    setup((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), positions);
    uint32_t e = 0;
    if (atoi(argv[8])) {                     // prime: fill every slot before the first bank
        for (; e < nslots; ++e) {
            write_frame(ring_slot(&ring, e), e);
            ring_publish(&ring, e);
        }
    }
    int busy = 0, blocked = 0;
    uint8_t *slot = NULL;
    uint64_t ready = 0, done = 0;
    const uint64_t banks = banks_for(positions);
    for (uint64_t k = 0; k < banks; ++k) {
        const uint64_t now = k < 2 ? 0 : (k - 1) * STEP;   // fill time: one period before play
        for (;;) {
            if (busy) {
                if (done > now) {
                    break;
                }
                write_frame(slot, e);
                ring_publish(&ring, e++);
                busy = 0, ready = done;
            }
            if (blocked) {
                ready = now, blocked = 0;
            }
            if (!(slot = ring_slot(&ring, e))) {
                blocked = 1;
                break;
            }
            memset(slot, POISON, length);
            busy = 1;
            done = ready + (every && e % every == every - 1 ? burst : base);
        }
        fill_bank(k);
    }
    check();
    return 0;
}

static int stop;

static void pause_ns(unsigned *seed, unsigned chance, long ns)
{
    if (rand_r(seed) % chance == 0) {
        nanosleep(&(struct timespec){0, ns}, NULL);
    }
}

static void *encoder(void *arg)
{
    unsigned seed = *(unsigned *)arg;
    for (uint32_t e = 0; !__atomic_load_n(&stop, __ATOMIC_RELAXED); ++e) {
        uint8_t *slot;
        while (!(slot = ring_slot(&ring, e))) {
            if (__atomic_load_n(&stop, __ATOMIC_RELAXED)) {
                return NULL;
            }
            pause_ns(&seed, 2, 20000);
        }
        memset(slot, POISON, length);
        for (uint32_t i = 0; i < length; ++i) {
            slot[i] = frame_byte(e, i);
            if (i % 1024 == 0) {
                pause_ns(&seed, 64, 50000);
            }
        }
        ring_publish(&ring, e);
        pause_ns(&seed, 8, 2000000);
    }
    return NULL;
}

static int threads(int argc, char **argv)
{
    if (argc != 6) {
        return 2;
    }
    const uint32_t positions = (uint32_t)atoi(argv[4]);
    unsigned seed = (unsigned)atoi(argv[5]), fill_seed = seed * 7 + 1;
    setup((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), positions);
    pthread_t t;
    pthread_create(&t, NULL, encoder, &seed);
    while (__atomic_load_n(&ring.published, __ATOMIC_ACQUIRE) < nslots) {
        nanosleep(&(struct timespec){0, 100000}, NULL);
    }
    const uint64_t banks = banks_for(positions);
    for (uint64_t k = 0; k < banks; ++k) {
        fill_bank(k);
        pause_ns(&fill_seed, 4, 30000);
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
    pthread_join(t, NULL);
    check();
    return 0;
}

int main(int argc, char **argv)
{
    const int rc = argc > 1 && !strcmp(argv[1], "sim")       ? sim(argc, argv)
                   : argc > 1 && !strcmp(argv[1], "threads") ? threads(argc, argv)
                                                             : 2;
    if (rc == 2) {
        fprintf(stderr, "usage: see the comment at the top of tests/ring_host.c\n");
    }
    return rc;
}
