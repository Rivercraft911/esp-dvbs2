#!/usr/bin/env python3
"""Host-only quality sweep and 80 MS/s budgets. JSON lines on stdout; no devices opened.

python host/shaper_budget.py --disassembly notes/shaper.dis --qemu notes/qemu.log
Capture once into a new results directory. The cycle model reads assembled instructions;
quality uses cyclic 16384-symbol sequences, seed 7. See notes/shaper-next.md for derivations.
"""
import argparse
from dataclasses import replace
import hashlib
import json
import pathlib
import subprocess

import shaper as s
import shaper_cycles as cycles

BANK, LEAD, TAIL, CPU, FS = 16380, 64, 2048, 240_000_000, 80_000_000
STRIDE = BANK-LEAD-TAIL
DEADLINE = STRIDE * CPU / FS
# Existing measurements, not new measurements. Long-copy bandwidth need not predict a small copy.
COPY_CPB = 4250160 / 1048576  # results/2026-10-16-shaper/runs.jsonl:7
CALL_ALLOWANCE = 1000        # assumed wrapper/queue/scheduling cost; n4 kernel tails modelled


def scenario(name, kernel_cycles, sps, producer_cycles=0, copy_bytes=0, deadline=DEADLINE):
    nominal = kernel_cycles + CALL_ALLOWANCE + copy_bytes * COPY_CPB
    stress = kernel_cycles * 1.22 + CALL_ALLOWANCE + copy_bytes * COPY_CPB
    return dict(kind='fill_estimate', scenario=name, samples_per_symbol=sps,
                kernel_cycles=kernel_cycles, assumed_control_cycles=CALL_ALLOWANCE,
                estimated_copy_cycles=copy_bytes*COPY_CPB, producer_cycles=producer_cycles,
                deadline_cycles=deadline, fill_cycles=nominal, slack_cycles=deadline-nominal,
                stress_22pct_cycles=stress, stress_slack_cycles=deadline-stress)


def budget(rows):
    c = {r['kernel']:r for r in rows}
    # Calibrate this instruction family against its recorded radio-off n8 bank benchmark.
    calibration = 29088 / c['shaper_fill8_pie']['cycles']
    c4 = c['shaper_fill4_pie']['worst_bank_kernel_cycles'] * calibration
    cr = c['shaper_fill4_rows_pie']['worst_bank_kernel_cycles'] * calibration
    c5 = c['shaper_fill5_pie']['cycles'] * calibration
    stage = c['shaper_rows4']['cycles']
    yield dict(kind='calibration', source='results/2026-10-16-shaper/runs.jsonl:10',
               board_cycles=29088, model_cycles=c['shaper_fill8_pie']['cycles'], factor=calibration,
               stress_source='results/2026-10-16-fec-limit/README.md, Cycles per frame')
    yield scenario('n4 PSRAM copy on fill core',c4,4,copy_bytes=BANK//4+8)
    legacy = c4 + 14395 + (4103-420)*COPY_CPB
    yield dict(kind='legacy_fill_estimate',fill_cycles=legacy,slack_cycles=DEADLINE-legacy,
               source='results/2026-10-16-dryrun/README.md, Timing on the board; shape4 notes')
    yield scenario('n4 producer-staged bytes',c4,4,producer_cycles=(BANK//4+8)*COPY_CPB)
    yield scenario('n4 internal frame source',c4,4)
    yield scenario('n4 producer-staged row pointers',cr,4,producer_cycles=stage)
    yield scenario('n5 internal bytes, existing geometry (lower bound)',c5,5)
    # 60 lead + 2040 tail gives stride 14280 divisible by 20 (four five-sample symbols).
    yield scenario('n5 aligned geometry, internal bytes',c5,5,deadline=14280*CPU/FS)
    for rate in (20_000_000,16_000_000):
        frame = 22194  # normal 8PSK with pilots, host/dvbs2.py plframe(), Table 11
        available = CPU*frame/rate
        for input_kind, encode in (('GCS',313000),('TS',367000)):
            yield dict(kind='encoder_budget', input=input_kind, symbol_rate=rate, symbols_per_frame=frame,
                       measured_encoder_max_rounded_cycles=encode, frame_budget_cycles=available,
                       remaining_cycles=available-encode, core_fraction=encode/available,
                       payload_bps=(53840-80)*rate/frame, # 5/6 kbch, mode 15 in dvbs2.c
                       minimum_cycles_to_remove=max(0, encode-available),
                       source='results/2026-10-16-fec-limit/README.md, Cycles per frame')
    for name,pulse in (('n4 bytes',c4),('n4 rows',cr),('n5 bytes',c5)):
        sps = 5 if name.startswith('n5') else 4
        rate = FS/sps
        stage_per_sec = (8*rate if name == 'n4 rows' else 0)
        fill_per_sec = (pulse+CALL_ALLOWANCE)*FS/STRIDE
        encode_per_sec = 313000*rate/22194
        yield dict(kind='two_core_estimate', scenario=name, symbol_rate=rate,
                   encoder_cycles_per_second=encode_per_sec, fill_cycles_per_second=fill_per_sec,
                   row_stage_cycles_per_second=stage_per_sec,
                   total_cores=(encode_per_sec+fill_per_sec+stage_per_sec)/CPU)


def records(disassembly, qemu):
    files = ['host/shaper.py','host/shaper_budget.py','host/shaper_cycles.py',
             'firmware/main/shaper.c','firmware/main/shaper_pie.S','firmware/main/shaper_taps.h',
             'tests/shaper_pie.c']
    yield dict(kind='provenance', evidence='host model and QEMU only; no board or SDR',
               base_revision=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
               source_sha256={f:hashlib.sha256((s.ROOT/f).read_bytes()).hexdigest() for f in files},
               seed=7, cyclic_symbols=16384, mask_grid_fn=1e-4,
               numpy_version=s.np.__version__,
               qemu_log_sha256=hashlib.sha256(pathlib.Path(qemu).read_bytes()).hexdigest() if qemu else None,
               disassembly_sha256=hashlib.sha256(pathlib.Path(disassembly).read_bytes()).hexdigest())
    for name in s.ON_DEVICE:
        p = s.PROFILES[name]
        for eq in (False,True):
            yield dict(kind='quality_model',**s.report_profile(replace(p,equalized=eq)))
        for span in (11,13):
            longer = replace(p,span=span)
            yield dict(kind='truncation_model',**s.report_profile(longer))
        for span in (9,11,13,33):
            taps = s.corrected_rrc(p.alpha,p.n,span)
            yield dict(kind='floating_pulse_model',profile=name,span_symbols=span,
                       mer_db=s.pulse_mer_taps(taps,p.n,p.alpha))
    r = list(cycles.report(disassembly))
    yield from r
    yield from budget(r)
    yield dict(kind='membench_expected',checks=s.expected_checks())
    if qemu:
        for line in pathlib.Path(qemu).read_text().splitlines():
            if line.startswith('@{"test":"shape'):
                yield dict(kind='qemu_correctness',**json.loads(line[1:]))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--disassembly',required=True)
    ap.add_argument('--qemu')
    a = ap.parse_args()
    for record in records(a.disassembly,a.qemu):
        print(json.dumps(record,sort_keys=True),flush=True)
