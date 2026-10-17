# Round two, offline

Firmware source: `ba93926` on `exp/max-astra`, following the `exp/max-opus` merge
`8cb6337`. Baseline for the linked-size comparison is `aa75bab`, built from an
archive with the same ESP-IDF v6.1 configuration. No board, serial port or radio
was used. All cycles here are models or explicitly identified older measurements.

The complete radio-off `20ms6` path uses a six-symbol pulse and a 16 KiB table.
Its assembled kernel models at 33,042 cycles/16,380 samples. The complete fill
model is 38,750 cycles; adding the prior model's 1,357-cycle unexplained residual
gives 40,107 cycles, very close to the 40,137-cycle 15% gate. The gate has not
been tested on silicon. 26.67 Msym/s was not achieved.

`runs.jsonl` contains provenance, instruction/whole-fill models, budget cases,
two independent receiver results, linked section deltas and the check summary.
The source of the 167k/185k encoder inputs is
`results/2026-10-16-next2/runs.jsonl`. The 133.5k case substitutes a hypothetical
10k ingress cost for 43.5k of synthetic payload generation; it is not measured.

Offline validation:

- All 30 host test scripts pass, including 140 FEC reference CRCs and legacy dump CRCs.
- QEMU: 324 legacy table/kernel cases, 162 new cases, 2,560 sample-glue cases,
  26,114 pack/unpack cases, 864 word-copy cases, and 24 block4 cases pass.
- Encoder QEMU: 140 CRCs, 14 zero-mismatch PIE checks and 192 guarded packed frames.
  The packed check fills the workspace tail with different bytes before each
  encode; both the tail and encoded output remain correct.
- Native stream checks cover startup, allocation failures, repeated commands and
  all physical offsets over full 1,365-bank and 819-bank wrap cycles. Every output
  word is compared with the plain encoder plus portable shaper.
- LeanDVB recovers 37 contiguous frames each for 2/3 and 5/6, no payload errors,
  LDPC corrections/check failures or BCH failures. Digital MER is 45.01/45.02 dB;
  these host captures do not include DAC hold or RF effects.
- Firmware and both QEMU apps build with ESP-IDF v6.1. Component-manager process
  discovery is blocked by the sandbox; `IDF_COMPONENT_MANAGER=0` avoids that
  optional component manager. No managed dependencies are required.

Reproduce the main additions with `python3 tests/test_round2.py`,
`python3 tests/test_stream.py`, and the shaper/encoder QEMU recipes in `tests/`.
Pass a shaper QEMU log to `tests/test_round2.py` and disassembly to
`host/round2.py --disassembly <file>`; QEMU CCOUNT is never timing evidence.
Build logs, raw QEMU logs, disassembly, baseline build and captures remain in
ignored `notes/`; their log hashes are in the provenance record.

[Design, RAM account and preregistered coordinator commands](../../docs/round2.md).
The coordinator must create a new results directory for silicon measurements.
