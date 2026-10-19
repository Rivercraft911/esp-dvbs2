# 8PSK startup

The supplied fatal run peaks at sample 417,739,472. The ID ends at
417,600,000; the first bank intersecting DVB starts at 417,597,812. The peak
is bank index 9, not the first bank. `psk8_handoff.c` reproduces that exact
bank with two startup dummies after data frame 2, including its return to
data, and checks every word against Python for MODCODs 13/15/17.

Candidate cause: cold PSRAM dummy descriptors. Pulse tables and queue pages
are internal; renderer instructions are already locked. The 13,376-byte dummy
buffer was only warmed before the 5.22-second ID. The candidate preloads and
locks its owned, aligned data-cache lines after Wi-Fi admission, then unlocks
before free. Both dummy variants and their prefetch guards fit; 19,392 bytes
of the 32 KiB data cache remain available. This hypothesis and the encoder's
remaining cache budget still need silicon validation. No silicon result is
claimed here.

`dvbpace` records the first 32 complete physical banks intersecting DVB,
including both callbacks. `startup.columns` describes each row: fill cycles,
shared PSRAM miss delta, and counter-snapshot/store overhead outside the fill
timer. These counters include both CPUs and interrupts; they cannot attribute
a miss to the consumer alone. Original epoch/deadline checks remain active,
including diagnostic overhead. Trace storage uses the existing radio task's
stack and an extra 400 PSRAM bytes, copied out after playback; no timed PSRAM
writes, logging, allocation, or additional internal-memory reservation.

Coordinator: after the usual build/flash, from the repo root:

```sh
python3 notes/psk8-start.py --repeats 3
```

This takes about 3.5 minutes: nine resets, Wi-Fi up, radio off. It appends raw
records to `notes/evidence/psk8-start-fixed.jsonl` and prints first-bank fill,
startup peak/index, overall max, late/fatal counts, trace overhead, PSRAM
misses and Wi-Fi minimum heap. It fails on errors, incomplete traces, any
late/fatal, max fill above 43,919, or Wi-Fi heap below 20,000. Use `--repeats 10`
for thirty resets (~12 minutes). Commit 6bdc0e2 is the trace-only comparison;
its trace additionally includes shared instruction misses. Compare bank 9
and neighboring banks, max fill/late counts, encoder live maximum, and Wi-Fi
heap. A cache-lock API success alone is not timing qualification.

Host validation: all 29 test programs pass; CRCs 13/15/17 remain
2061900610 / 3631096604 / 1787558147. Residency tests cover timeout, ROM errors,
validation and cleanup. The linked default fill/PIE addresses, 1,632-byte
instruction lock, IRAM reservation and shaper-store address match main.
Build passes with IDF v6.1; this sandbox needs `IDF_COMPONENT_MANAGER=0`
because the unused component manager's process scan is restricted.
