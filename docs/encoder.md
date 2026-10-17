# Encoder experiments, radio off

`fecbench pie` uses a byte BCH recurrence for t = 10/12 and register-based
nibble slicing for t = 8. The prepared context sizes and canonical workspace
stay unchanged. These are candidates for board measurement. QEMU establishes
bit accuracy; its printed CCOUNT values do not establish speed.

Warm pipeline model, cycles per normal 8PSK frame with pilots:

| Rate | Original GCS | Candidate GCS | Candidate TS |
|---|---:|---:|---:|
| 2/3 | 174,144 | 129,177 | 155,245 |
| 5/6 | 191,505 | 136,696 | 169,251 |
| 9/10 | 182,286 | 127,600 | 162,791 |

These count actual QEMU instruction traces with interrupts excluded and apply
TRM section 1.7.1 interlocks. They omit flash/cache misses, PSRAM and contention.
Applying each changed stage's speed ratio to that mode's saved board stage
median projects GCS totals of 161k, 237k and 146k cycles respectively. Those are
estimates, not board results or bounds. In particular the former 5/6 model
underpredicted silicon much more than 2/3. Internal context sizes are unchanged;
additional code uses 864 bytes of IRAM, and tables add 20,480 bytes of flash.

For generator degree r bits, byte v, and remainder R:

```
T[v] = v x^r mod g(x)
R' = ((R << 8) mod x^r) XOR T[(R >> (r - 8)) XOR v]
```

Each of the two byte tables is 8,192 bytes in flash. The generator derives them
from EN 302 307-1 V1.4.1 clause 5.3.1, Table 6a, pp. 23-24. The two zeroed PIE
registers hold the MSB-first remainder, and one `EE.SRCI.2Q` shifts it per byte
([ESP32-S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
section 1.8.53, p. 129). Partial words are consumed by the byte kernel too.

The nibble variant keeps eight table bases in scalar registers. Vector loads
ignore their four low address bits (TRM sections 1.8.88-89, pp. 164-165), so
seven index shifts per word disappear. The t = 8 kernel also schedules each
XOR after the next index extraction to hide load latency.

The 8PSK mapper keeps each pair's index fields at offsets 0, 4 and 8. Masking
with `0x333` replaces repacking six consecutive bits. A sparse 4,096-byte flash
table replaces the 256-byte table; only 64 words are addressed. LDPC combines
two window shifts with loads using `EE.SRC.Q.LD.IP` (TRM section 1.8.50, p. 126).

The variants all emit `impl:"pie"`; `bch_variant` identifies the selection.
Each run checks 14 PIE stages against C and emits 140 reference frame CRCs.

| Command | BCH | 8PSK mapping |
|---|---|---|
| `fecbench legacy` | original nibble kernel | original compact table |
| `fecbench nibble` | register-based nibble kernel | sparse table |
| `fecbench pie` | byte kernel, register nibble for t = 8 | sparse table |
| `fecbench sram` | same, byte table copied into scratch every frame | sparse table |

All variants use the new LDPC window loads. `sram` times the copy inside BCH.
It overwrites LDPC scratch before LDPC begins; it adds no context allocation.
The 8PSK 2/3 context remains 14,736 bytes on the S3. Additional assembly and C
instructions consume IRAM, and the additional tables consume flash.

The coordinator can collect about one minute of each variant with this Bash
script after building/flashing the branch through the normal workflow. None
of these commands starts the radio or accesses the SDR. Use a fresh run folder.

```bash
set -e
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
run_dir=results/2026-10-16-rd-encoder-board
mkdir "$run_dir"
for variant in legacy nibble pie sram; do
    stop_at=$((SECONDS + 60))
    while (( SECONDS < stop_at )); do
        "$PY" host/board.py "fecbench $variant" --timeout 120 \
            --out "$run_dir/$variant.jsonl"
    done
    "$PY" tests/test_fecbench.py "$run_dir/$variant.jsonl" --correctness-only
done
```

Correctness pass: every run completes with 140 matching CRCs, 14 zero-mismatch
PIE checks and `pie_mask:15`. The existing strict timing report remains available
without `--correctness-only`; its old 250k-cycle TS threshold is separate from
this A/B experiment. Do not interpret the legacy run's known TS timing failure
as a CRC failure.

Performance pass: compare per-mode, per-input medians and maxima in the records.
Byte BCH should take less than 75% of legacy BCH for MODCOD 13/15; scheduled
nibble BCH should take less than 75% for MODCOD 17. Sparse mapping should take
less than 85% of compact mapping. Reject any candidate with a regression above
5% in another mode/input, or a failed allocation. Keep `sram` only if its BCH
median, including the copy, beats `pie` by at least 5% without worse maxima.

The one-core budgets are 199,721 cycles/frame at 26.67 Msym/s and 266,328 at
20 Msym/s: `240 MHz * 22,194 symbols/frame / symbol_rate`. Allow ingress,
interrupt and scheduling headroom. The local transmitter still writes PSRAM
frame slots; the faster internal-slot stream from the coordinator's separate
branch must be integrated before judging its 231k/325k-cycle stream baselines.
No complete 20 or 26.67 Msym/s transmitter is qualified here.
