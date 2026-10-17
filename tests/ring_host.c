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
// Environment: RING_SPS, RING_DUMMY (filler length), RING_SPLIT (separate slots), RING_CACHE
// (overlap cache) and RING_PACKED (frames two phases per byte; frame values, POISON and FILLER
// become nibbles 1..12, 14 and 15).

#define _POSIX_C_SOURCE 200809L   // rand_r, nanosleep under -std=c11 on glibc

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ring.h"

#define BANK 16380
#ifndef LEAD                      // the 40 MS/s geometry; test_ring.py also builds 80 MS/s's
#define LEAD 64
#define TAIL 2048
#endif
#define STEP (BANK - LEAD - TAIL)
static unsigned sps = 40;
#define SPS sps
#define HISTORY 8
#define WINDOW (HISTORY + 4096)
#define POISON 0xEE
#define FILLER 0xFA
#define UNSEEN 0xFFFF

static ring_t ring;
static uint32_t length, nslots, dummy, recorded;
static struct position { uint64_t start, end; uint32_t choice; } *events;
static uint16_t *seen;
static uint64_t seen_symbols;
static uint8_t *separate[RING_MAX_SLOTS];
static uint8_t overlap[HISTORY + 2 + (BANK - STEP + LEAD + 3) / 4];
static int packed;
static int codes, reuse;
static uint64_t read_end;

static uint8_t frame_byte(uint32_t e, uint32_t i)
{
    uint32_t h = e * 2654435761u ^ (i * 40503u + 0x9e3779b9u);
    h ^= h >> 15, h *= 0x2c1b3c6du, h ^= h >> 12;
    return (uint8_t)(1 + h % (packed ? 12 : 200));   // never POISON, FILLER or 0
}

static uint8_t poison(void) { return packed ? 0xE : POISON; }
static uint8_t filler_value(void) { return packed ? 0xF : FILLER; }
static size_t bytes(uint32_t symbols) { return packed ? symbols / 2 : symbols; }

static void fail(const char *what, long long a, long long b)
{
    printf("FAIL %s %lld %lld\n", what, a, b);
    exit(1);
}

// One fill call, as dvbtx.c makes it, then the overlap and content checks.
static void fill(uint64_t first, size_t count)
{
    if (reuse && first < read_end) {
        const size_t cached = read_end - first < count ? (size_t)(read_end - first) : count;
        first += cached, count -= cached;
    }
    if (!count) return;
    read_end = first + count;
    uint8_t buffer[WINDOW + 1];
    uint8_t *win = buffer + (first / SPS & 1);    // dvbtx.c's parity rule for packed reads
    const int nsym = ring_window(&ring, win, WINDOW, first, count, SPS, HISTORY, BANK);
    if (nsym < 0) {
        fail("ring_window", (long long)first, (long long)count);
    }
    while (recorded < ring.next_position) {
        const unsigned i = recorded % RING_CHOICES;
        events[recorded++] = (struct position){ring.start[i], ring.end[i], ring.choice[i]};
    }
    const int64_t s0 = (int64_t)(first / SPS) - HISTORY;
    for (int j = 0; j < HISTORY + nsym; ++j) {
        const int64_t i = s0 + j;
        if (codes) {
            if (win[j] & 15) fail("unshifted code", i, win[j]);
            win[j] >>= 4;
        }
        if (i < 0) {
            if (win[j]) {
                fail("history before symbol 0", i, win[j]);
            }
            continue;
        }
        if (win[j] == poison()) {
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
        if (!packed) {
            slot[i] = frame_byte(e, i);
        } else if (i & 1) {
            slot[i / 2] = (uint8_t)(frame_byte(e, i - 1) | frame_byte(e, i) << 4);
        }
    }
}

// Walk the chosen positions: frames in order, fillers exactly where underruns were counted.
static void check(void)
{
    for (uint32_t s = 0; s < nslots; ++s) {
        if (!separate[s]) continue;
        for (unsigned i = 0; i < 16; ++i) {
            if (separate[s][i] != POISON || separate[s][16 + bytes(length) + 16 + i] != POISON)
                fail("separate slot guard", s, i);
        }
    }
    uint32_t e = 0, fillers = 0;
    for (uint32_t q = 0; q < ring.next_position; ++q) {
        const struct position *p = events + q;
        const int kind = p->choice == RING_FILLER;
        if (!kind && p->choice != e) {
            fail("frame selection out of order", p->choice, e);
        }
        for (uint64_t i = p->start; i < p->end && i < seen_symbols; ++i) {
            const uint16_t v = seen[i];
            if (v != UNSEEN && v != (kind ? filler_value() : frame_byte(e, (uint32_t)(i - p->start)))) {
                fail(kind ? "filler content" : "frame out of order", q, (long long)i);
            }
        }
        if (p->end - p->start != (kind ? dummy : length)) {
            fail("position length", q, (long long)(p->end - p->start));
        }
        kind ? ++fillers : ++e;
    }
    if (e != ring.next_frame || fillers != ring.underruns) {
        fail("frame or underrun count", e, fillers);
    }
    printf("ok positions %" PRIu32 " frames %" PRIu32 " underruns %" PRIu32 " min_lead %lld\n",
           ring.next_position, e, ring.underruns, (long long)ring.min_lead);
}

static void setup(uint32_t l, uint32_t s, uint32_t positions)
{
    length = l, nslots = s;
    dummy = getenv("RING_DUMMY") ? (unsigned)atoi(getenv("RING_DUMMY")) : l;
    sps = getenv("RING_SPS") ? (unsigned)atoi(getenv("RING_SPS")) : 40;
    packed = getenv("RING_PACKED") != NULL;
    codes = getenv("RING_CODES") != NULL;
    reuse = getenv("RING_REUSE") != NULL;
    uint8_t *slots = malloc((size_t)s * l), *filler = malloc(dummy + 16);
    memset(slots, POISON, (size_t)s * l);
    memset(filler, packed ? 0xFF : FILLER, dummy + 16);
    ring_init(&ring, slots, s, filler, l);
    if (getenv("RING_SPLIT")) {
        uint8_t *p[RING_MAX_SLOTS];
        for (uint32_t i = 0; i < s; ++i) {
            // 16 guard bytes, the slot, 16 bytes a vector read may touch, 16 guard bytes.
            separate[i] = malloc(bytes(l) + 48);
            memset(separate[i], POISON, bytes(l) + 48);
            p[i] = separate[i] + 16;
        }
        ring_init_slots(&ring, p, s, filler, l);
        // dvbtx.c's OVERLAP_SYMBOLS: history, the longest re-read, one cut symbol at each end.
        const unsigned margin = getenv("RING_CACHE_MARGIN") ? (unsigned)atoi(getenv("RING_CACHE_MARGIN")) : 2;
        if (getenv("RING_CACHE"))
            ring_cache(&ring, overlap, reuse ? HISTORY + 2 :
                       HISTORY + margin + (BANK - STEP + LEAD + SPS - 1) / SPS);
    }
    if (packed) {
        if (codes) ring_packed_codes(&ring);
        else ring_packed(&ring);
    }
    ring_filler_length(&ring, dummy);
    seen_symbols = (uint64_t)(positions + 2) * l;
    events = calloc(seen_symbols / (dummy < l ? dummy : l) + 8, sizeof *events);
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
    sps = getenv("RING_SPS") ? (unsigned)atoi(getenv("RING_SPS")) : 40;
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
            memset(slot, POISON, bytes(length));
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
        memset(slot, POISON, bytes(length));
        for (uint32_t i = 0; i < length; ++i) {
            if (!packed) {
                slot[i] = frame_byte(e, i);
            } else if (i & 1) {
                slot[i / 2] = (uint8_t)(frame_byte(e, i - 1) | frame_byte(e, i) << 4);
            }
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
