# APSK reference and independent file loopback

All 56 requested APSK host loopbacks recovered the complete application file, with
5,875,136 compared bits and zero differences. This is simulated baseband interoperability,
not an RF measurement or a sensitivity/throughput result. No hardware or firmware was used.

Normal frames, pilots on, PL scrambling code 0. Each mode has eight cases: 1 and 5 Msym/s,
RRC roll-offs 0.20 and 0.35, clean and impaired. Capture rates are 4 and 10 MS/s respectively.
Impaired cases use 26 dB sample SNR, carrier offset +0.01 Rs (+10 or +50 kHz), clock error
+50 ppm, seed 20261016. The RRC pulse spans 12 symbols. LeanDVB uses RRC rejection 40 dB,
`--fastdrift`, and its internal hard-decision LDPC decoder with `--ldpc-bf 100`.

| Mode (MODCOD) | Unique bytes per case | Cases passed | Valid BBFRAMEs | BCH failures | Ideal Es/N0, dB | Minimum modelled margin, dB |
|---|---:|---:|---:|---:|---:|---:|
| 16APSK 2/3 (18) | 10,877 | 8/8 | 80 | 0 | 8.97 | 20.04 |
| 16APSK 3/4 (19) | 12,219 | 8/8 | 80 | 0 | 10.21 | 18.80 |
| 16APSK 5/6 (21) | 13,577 | 8/8 | 80 | 0 | 11.61 | 17.40 |
| 16APSK 9/10 (23) | 14,665 | 8/8 | 80 | 0 | 13.13 | 15.88 |
| 32APSK 3/4 (24) | 12,219 | 8/8 | 69 | 0 | 12.73 | 16.28 |
| 32APSK 5/6 (26) | 13,577 | 8/8 | 67 | 1 | 14.28 | 14.73 |
| 32APSK 9/10 (28) | 14,665 | 8/8 | 69 | 0 | 16.05 | 12.96 |

Each signal has four training frames, three application frames (last field 137 bytes), and
eight tail frames. The complete expected application file must occur contiguously in the
output. Endpoint loss is allowed. All clean cases also have zero BCH failures and zero
residual LDPC checks. There were 526 FEC attempts, 525 valid BBFRAMEs, 2,548 LDPC bit flips,
three BCH corrections and 1,168 residual LDPC checks in total. These counts are decoder
actions/syndromes, not channel bit errors. The impaired 32APSK 5/6, alpha 0.20, 5 Msym/s
case includes one rejected attempt with incorrect MODCOD 27; accepted frames indicate 26.
Its full application file still matches. Impaired 32APSK 3/4 at alpha 0.20 and 1 Msym/s
also has two residual checks without a BCH failure.

The Es/N0 model is `sample_SNR + 10 log10(Fs_actual/Rs)` dB, with
`Fs_actual = Fs (1 + 50e-6)` samples/s and Rs in symbols/s. It assumes ideal matched filtering
of the unit-average-energy symbols. Thus the impaired cases have approximately 32.02 dB
at 1 Msym/s and 29.01 dB at 5 Msym/s. Margins subtract the Table 13 values. Table 13 describes
simulation at packet error ratio 1e-7 with 50 fixed-point LDPC iterations, perfect timing and
carrier recovery, no phase noise and AWGN (clause 6, p. 36; text lines 2106-2117, 2138-2148).
This short run does not measure that error ratio, and LeanDVB's decoder is different.

`stress.jsonl` retains six additional trials at 5 Msym/s, alpha 0.20, RRC rejection 40 dB,
for 32APSK 3/4 and 9/10 at sample SNRs 20, 26 and 32 dB. At 20 dB, 3/4 lost the expected
file (four BCH failures); 9/10 recovered it with one BCH failure. Both higher SNRs recovered
both files with zero BCH failures. This receiver/configuration has limits well above the
ideal threshold; a successful high-margin loopback is not a receiver sensitivity claim.
Preliminary RRC-rejection-20 trials also failed on clean 32APSK. Filter length depends on
rejection and roll-off in LeanDVB `src/apps/leandvb.cc:555-574`. Eight tail frames accommodate
its maximum-frame input lookahead (`src/leansdr/dvbs2.h:482-488`).

## Standard rules and normalization

Source: [ETSI EN 302 307-1 V1.4.1 (2014-11)](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).
Exact line citations below refer to the supplied local text
`/Users/river/Documents/stanford/SSI/RP2350_IQ_Benchmark/docs/sources/en_30230701v010401p.txt`,
SHA-256 `2de8493d9c2ae0f993c744243479670d0a5aa4188cf7cc2386eb1e58dd34df56`.

| Rule | Clause, printed page, exact text lines |
|---|---|
| Write interleaver columns, read rows, MSB first; reversal only for 8PSK 3/5 | 5.3.3, p. 26, 1486-1490 |
| 16APSK: 16,200 rows x 4 columns; 32APSK: 12,960 x 5 for normal frames | Table 8, p. 26, 1492-1499 |
| 16APSK: 4 inner + 12 outer points, admitted energy scalings, MSB-first grouping | 5.4.3, p. 28, 1626-1640 |
| All 16APSK bit labels and radius ratios | Figure 11 / Table 9, p. 29, 1649-1678 |
| 32APSK: 4 + 12 + 16 points, admitted energy scalings, grouping | 5.4.4, p. 29, 1682-1694 |
| All 32APSK bit labels and radius ratios | Figure 12 / Table 10, p. 30, 1702-1741 |
| 180 / 144 slots for normal 16APSK / 32APSK and frame length | Table 11, p. 31, 1794-1805 |
| MODCOD 18-23 / 24-28 | Table 12, p. 32, 1850-1865 |
| 36 unit-energy pilots after each 16 slots, excluding frame end | 5.5.3, p. 33, 1928-1938 |
| Scramble body including pilots, reset after header, multiply by exp(j R pi/2) | 5.5.4, pp. 33-34, 1943-1955, 1982-2021 |

Correction to the task wording: Tables 7a/7b are LDPC q values (text lines 1444 and 1465),
not interleaver dimensions. Table 8 defines the interleaver.

APSK does **not** require unit average energy exclusively. The standard also admits unit
outer radius (16APSK R2=1, 32APSK R3=1). This encoder chooses average energy E=1. Radii and
gammas below are dimensionless. With equiprobable labels:

```text
16APSK: E = (4 R1^2 + 12 R2^2)/16 = 1, R2 = gamma R1
        R1 = sqrt(4/(1 + 3 gamma^2))
32APSK: E = (4 R1^2 + 12 R2^2 + 16 R3^2)/32 = 1
        R2 = gamma1 R1, R3 = gamma2 R1
        R1 = sqrt(8/(1 + 3 gamma1^2 + 4 gamma2^2))
```

Both formulas give all radii equal to one when all gammas are one. Header and pilot symbols
also have energy one. A finite data frame need not have exactly unit mean energy because
its labels are not exactly equiprobable. No per-frame amplitude rescaling is applied.

| Code rate | 16APSK gamma | 32APSK gamma1, gamma2 |
|---|---:|---|
| 2/3 | 3.15 | unavailable |
| 3/4 | 2.85 | 2.84, 5.27 |
| 4/5 | 2.75 | 2.72, 4.87 |
| 5/6 | 2.70 | 2.64, 4.64 |
| 8/9 | 2.60 | 2.54, 4.33 |
| 9/10 | 2.57 | 2.53, 4.30 |

The unchanged framing rules yield 16,686 symbols for normal 16APSK with pilots (16,290
without) and 13,338 for 32APSK with pilots (13,050 without). `plframe_symbols` and
`transmit_symbols` carry complex amplitude and phase; the existing phase-byte API rejects
APSK. `iq.shape_symbols` and `host/iq.py --complex-symbols` preserve the radii. Short APSK
frames are deliberately unsupported by the frame/transmit API.

## Independent verification and reproduction

`tests/test_dvbs2_apsk.py` derives labels by bit-field/quadrant formulas, independently of
the implementation's ring-label lists. It checks every interleaver address, every point,
radius ratio and average energy at all 11 rates, 44 frames with pilots on/off and Gold codes
0/262141 using independent header/Gold formulas, and 11 two-frame GCS reversals with a final
13-bit field. The tests also preserve QPSK/8PSK output and reject unsupported modes.

New `tests/data/apsk_digests.json` fixtures came from separate transmitter harnesses:
[gr-dtv aee9fd3f79389c4282a98e8d62c8405c73fd91df](https://github.com/gnuradio/gnuradio/tree/aee9fd3f79389c4282a98e8d62c8405c73fd91df/gr-dtv)
and [LeanSDR 84c59e1c7a1a79338d5722d63f28640cc9d350f3](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3).
Both match BBFRAME, scrambled BBFRAME, BCH, LDPC and interleaver-label digests at all 11 rates,
pilots on/off, three consecutive TS frames each (44 external comparisons). The input is
200 packets from `tests/test_dvbs2.py:ts_input`, xorshift32 seed 2026. Bit digests use packed
MSB-first bits; interleaver digests use raw label bytes. Both use the first 128 SHA-256 bits.
The systematic FEC prefixes supply scrambled-BBFRAME and BCH stages.

gr-dtv's mapped and full PL complex coordinates also match digests rounded to a 1e-4 grid
(interleaved signed little-endian int32 I,Q); maximum unrounded error is 5.99e-8. LeanSDR's
complete PL output was compared numerically after division by its amplitude scale 75;
maximum error is 0.01872, below sqrt(2)/75. Its data coordinates truncate to signed integers
(`src/leansdr/sdr.h:299,609-612`, `dvbs2.h:359-371`), so this is not a floating-point bit-exact
claim. APSK decoding support is explicit in `sdr.h:456-531` and `dvbs2.h:240-251,1333-1343`.
The existing QPSK-only digest file was not relabelled as APSK evidence.

Base commit `5b54d8d` on `exp/apsk-ref`. `inputs.json` identifies all relevant source/fixture
hashes, tool versions and the unchanged local receiver binary/patch. The receiver is the
same separately built arm64 executable as the earlier decode run; its build was not modified.
Full harnesses, build commands, logs and exploratory details remain in ignored `notes/apsk.md`
and its companion files. No external implementation code is incorporated into the encoder.

```sh
python3 tests/test_dvbs2_apsk.py
python3 tests/test_iq.py
python3 tests/test_decode.py --apsk-loopback \
  --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  --out results/<new-run>-apsk-decode/runs.jsonl
```

All host tests and the original 40-case PSK loopback passed. The new offline tests run in CI;
independent receiver loopbacks require the local executable. No firmware build is needed
because firmware is unchanged. RF, board execution, APSK firmware, short APSK frames,
nonlinear distortion and decoding near Table 13 thresholds remain unverified.
