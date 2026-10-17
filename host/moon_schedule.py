#!/usr/bin/env python3
"""Two-core capacity model. Assembled issue costs, never silicon timing.

Disassembly: xtensa-esp32s3-elf-objdump -d --no-show-raw-insn libmain.a.
The optional handoff trace comes from tests/moon_handoff.c, QEMU
-d in_asm,exec,nochain. No QEMU CCOUNT is used.
"""
import argparse
import json
import math
import pathlib
import re

import shaper_cycles as c

BANK, CEILING = 47220, 40137


def handoff(trace, disassembly):
    functions = c.functions(disassembly)
    begin, end = (functions[n][0][0] for n in ('handoff_begin','handoff_end'))
    blocks, current, active, rows = {}, [], False, []
    previous, instructions, redirects = None, 0, 0
    for line in pathlib.Path(trace).open(errors='replace'):
        ins = re.match(r'^0x([0-9a-f]+):\s+(.*)',line)
        if ins:
            current.append((int(ins[1],16),ins[2]))
            continue
        if current:
            blocks[current[0][0]] = current
            current = []
        event = re.match(r'^Trace 1: .*?\[[^/]+/([0-9a-f]+)/',line)
        if not event:
            continue
        pc = int(event[1],16)
        if pc == end:
            assert active
            rows.append(dict(instructions=instructions,redirects=redirects,
                             issue_cycles=instructions+2*redirects))
            active = False
        if pc == begin:
            assert not active
            active, previous, instructions, redirects = True, None, 0, 0
        if active:
            block = blocks[pc]
            if previous is not None:
                # QEMU prints instruction bytes in the linked disassembly;
                # the next sequential PC is known from its instruction list.
                last, op = previous
                if op.split()[0].startswith(('b','j','call','ret')) and pc != last+(2 if op.split()[0].endswith('.n') else 3):
                    redirects += 1
            instructions += len(block)
            previous = block[-1]
    assert len(rows) == 32, len(rows)
    return dict(kind='handoff_trace',evidence='QEMU executed instructions, single issue plus redirects; no interlocks/cache/IRQ bound',
                calls=len(rows),instructions_min=min(r['instructions'] for r in rows),
                instructions_max=max(r['instructions'] for r in rows),
                issue_cycles_max=max(r['issue_cycles'] for r in rows))


def packing(code):
    """Find the compiled 4-symbol/5-symbol loops and quad bulk loop.

    Count the actual instructions, including taken scalar back edges. Prologue,
    quad alignment/tails and call redirects have explicit allowances below.
    Fail when a compiler changes the loop shapes instead of silently guessing.
    """
    bodies = {}
    for i,(address,ins) in enumerate(code):
        op = ins.split()[0].removesuffix('.n')
        if op not in ('bnez','loop','loopnez'):
            continue
        target = int(ins.split(',')[-1].strip(),16)
        if op.startswith('loop'):
            body = [(s,False) for a,s in code[i+1:] if a < target]
        else:
            body = [(s,a == address) for a,s in code if target <= a <= address]
        stores = sum(s.startswith('s16i') for s,_ in body)
        loads = sum(s.startswith('l8ui') for s,_ in body)
        if stores == 4:
            bodies[1] = c.replay(body)['cycles']/4
        elif stores == 1 and loads in (4,5):
            bodies[3 if loads == 4 else 2] = c.replay(body)['cycles']
    assert set(bodies) == {1,2,3}, bodies
    return bodies


def report(disassembly, dispatch=2396):
    functions = c.functions(disassembly)
    pack = packing(functions['shaper_moon_pack'])
    root = pathlib.Path(__file__).resolve().parents[1]
    enc = [json.loads(l) for l in (root/'results/2026-10-17-moon-encoder/runs.jsonl').read_text().splitlines()]
    for mode,name,symbols,num,den,share,kernel in (
            (0,'8PSK 26m667',22194,3,1,(1,3),'shaper_moon3_pie'),
            (3,'QPSK 33m333',33282,12,5,(2,7),'shaper_moon24qc_quads_pie'),
            (2,'8PSK 33m333',22194,12,5,(1,3),'shaper_moon24p_pie')):
        frame = symbols*3*num/den
        mc = 11 if mode == 3 else 17
        encoder = next(r['kernel_plus_input_estimate_cycles'] for r in enc if
                       r.get('kind') == 'instruction_model' and r['variant'] == 'candidate'
                       and r['modcod'] == mc and r['input'] == 'gcs')
        body = functions[kernel]
        one = c.replay(c.loop_stream(body,1))['cycles']
        step = c.replay(c.loop_stream(body,2))['cycles']-one
        base = one-step
        if mode == 3: step /= 4

        def bank(k,scale=1):
            start = k*15740-128 if k else 0
            previous = (k-1)*15740-128+16380 if k > 1 else 16380 if k else 0
            offset = start%16380
            at, count = start,16380-offset
            parts = [(at,count)]+([(at+count,offset)] if offset else [])
            helper = local = staged = copied = jobs = 0
            for at,count in parts:
                keep = max(0,768-at%16380) if at%16380 and count == 16380-at%16380 else 768
                fresh_at = max(at,previous)
                fresh = max(0,at+count-fresh_at)
                copied += .5*(count-fresh+min(fresh,keep))
                if not fresh:
                    continue
                skip = fresh_at%12
                cut = ((fresh*share[0]//share[1]+skip)//12)*12
                h = max(0,cut-skip)
                for which,n,phase in (('helper',h,skip),('local',fresh-h,0 if h else skip)):
                    if not n:
                        continue
                    groups = math.ceil((phase+n)/12)
                    # QPSK consumes quad bytes directly in the bulk loop. Its
                    # at-most-eight edge blocks use the packed API; 1024 cycles
                    # per chunk reserves edge packing and wrapper control.
                    # This allowance, like the staging/copy costs, is estimated.
                    cost = 1024 if mode == 3 else (groups+2)*pack[3 if mode == 0 else 2]+32
                    cost += groups*step*scale+3*base+144
                    if which == 'helper': helper += cost
                    else: local += cost
                staged += math.ceil((fresh*den/num+16)/(4 if mode == 3 else 1))
                jobs += bool(h)
            # Ring copy one cycle/byte, sample copies half a cycle/word,
            # tick 1068/core/bank, control 1000. These are assumptions.
            core0 = helper+encoder*BANK/frame+jobs*dispatch+1068
            core1 = max(local,helper)+staged+copied+jobs*dispatch+1068+1000
            return dict(bank=k,core0_cycles=math.ceil(core0),core1_cycles=math.ceil(core1),
                        helper_cycles=math.ceil(helper),local_cycles=math.ceil(local),jobs=jobs,
                        staging_cycles=staged,sample_copy_cycles=copied)

        def worst(scale=1):
            rows = [bank(k,scale) for k in range(1,820)]
            row = max(rows,key=lambda r:max(r['core0_cycles'],r['core1_cycles'])).copy()
            for key in ('core0_cycles','core1_cycles'):
                row[key] = max(r[key] for r in rows)
            row['helper_bank_max_cycles'] = max(r['helper_cycles'] for r in rows)
            return row
        zero = worst(0)
        kernel_only_sufficient = max(zero[c] for c in ('core0_cycles','core1_cycles')) <= CEILING
        low,high = 0.,1.
        for _ in range(40):
            mid = (low+high)/2
            if max(worst(mid)[c] for c in ('core0_cycles','core1_cycles')) <= CEILING: low = mid
            else: high = mid
        row = worst()
        yield dict(kind='two_core_model',mode=name,evidence='uncalibrated issue/capacity model; not Wi-Fi-up silicon',
                   frame_cycles=frame,encoder_cycles=encoder,encoder_bank_cycles=encoder*BANK/frame,
                   kernel_cycles_per_block=step,pack_cycles_per_block=0 if mode == 3 else pack[3 if mode == 0 else 2],
                   helper_share=list(share),dispatch_allowance_cycles_per_job=dispatch,
                   target_cycles=CEILING,deadline_cycles=BANK,
                   ideal_fill_ceiling_cycles=math.floor(2*CEILING-encoder*BANK/frame),**row,
                   closes=max(row['core0_cycles'],row['core1_cycles']) <= CEILING,
                   kernel_only_sufficient=kernel_only_sufficient,zero_kernel=zero,
                   kernel_cycle_reduction_percent=100*(1-low) if kernel_only_sufficient else None,
                   required_kernel_cycles_per_block=step*low if kernel_only_sufficient else None,
                   startup=bank(0))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('disassembly',type=pathlib.Path)
    p.add_argument('--handoff-trace',type=pathlib.Path)
    p.add_argument('--handoff-disassembly',type=pathlib.Path)
    a = p.parse_args()
    dispatch = 2396
    if a.handoff_trace:
        measured = handoff(a.handoff_trace,a.handoff_disassembly)
        print(json.dumps(measured,sort_keys=True))
        dispatch = max(dispatch,measured['issue_cycles_max'])
    for row in report(a.disassembly,dispatch): print(json.dumps(row,sort_keys=True))
