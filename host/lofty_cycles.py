#!/usr/bin/env python3
"""Replay QEMU executed instructions, never CCOUNT. TRM v1.8 1.7/Table 1.7-2.

Single issue, load defines M=2, ALU defines E=1, taken redirects cost two.
No cache/bus stalls or silicon interrupt bound. Division reserves 11 cycles.
PIE gather dependencies are per 32-bit lane, not per vector register.
"""
import argparse
import json
import math
import pathlib
import re
import shaper_cycles as base


def operands(ins):
    op,*tail = ins.split(None,1)
    op = op.removesuffix('.n')
    a = [x.strip() for x in tail[0].split(',')] if tail else []
    reg = lambda r: re.fullmatch(r'[aq][0-9]+',r)
    if op == 'ee.ldxq.32':
        return [(a[1]+'.'+str(int(a[4])//2),1),(a[2],1)],[(a[0]+'.'+a[3],2)]
    if op == 'ee.movi.32.a': return [(a[0]+'.'+a[2],1)],[(a[1],1)]
    if op == 'ee.movi.32.q': return [(a[1],1)],[(a[0]+'.'+a[2],1)]
    if op == 'ee.notq': return [(a[1],1)],[(a[0],1)]
    if op == 'ee.vld.l.64.ip':
        return [(a[1],1)],[(a[0]+'.0',2),(a[0]+'.1',2),(a[1],1)]
    if op == 'ee.zero.q': return [],[(a[0],1)]
    if op == 'ee.vadds.s16': return [(a[1],1),(a[2],1)],[(a[0],1)]
    if op.startswith(('ee.vzip.','ee.vunzip.','ee.srci.2q')):
        return [(a[0],1),(a[1],1)],[(a[0],1),(a[1],1)]
    if op == 'ee.src.q.qup':
        return [(a[1],1),(a[2],1),('SARB',1)],[(a[0],1),(a[1],1)]
    if op == 'ee.src.q.ld.ip':
        return [(a[1],1),(a[3],1),(a[4],1),('SARB',1)],[(a[0],1),(a[3],2),(a[1],1)]
    if op.startswith(('ee.vld.128.xp','ee.vst.128.xp')):
        uses,defs = base.operands(op.replace('.xp','.ip')+' '+', '.join(a))
        return uses+[(a[2],1)],defs
    if op == 'ee.ld.128.usar.ip':
        return [(a[1],1)],[(a[0],2),(a[1],1),('SARB',1)]
    if op in ('ee.vsl.32','ee.vsr.32','ee.vmul.u8'):
        return [(r,1) for r in a[1:] if reg(r)]+[('SAR',1)],[(a[0],2 if 'vmul' in op else 1)]
    if op in ('ssai','ssa8l'): return [(r,1) for r in a if reg(r)],[('SAR',1)]
    if op in ('ssl','ssr'): return [(a[0],1)],[('SAR',1)]
    if op in ('sll','srl','sra','src'): return [(r,1) for r in a[1:] if reg(r)]+[('SAR',1)],[(a[0],1)]
    if op in ('neg','xor','addmi','subx4','muluh','minu','saltu','nsau','moveqz','movnez'):
        return [(r,1) for r in a[1:] if reg(r)],[(a[0],1)]
    if op in ('quos','quou','remu'): return [(r,1) for r in a[1:] if reg(r)],[(a[0],11)]
    if op == 'l32e': return [(a[1],1)],[(a[0],2)]
    if op == 's32e': return [(a[0],1),(a[1],1)],[]
    if op in ('memw','rsync','rfwo','rfwu','rfe','ret','rotw') or op.startswith(('call','j')): return [],[]
    if op.startswith(('xsr.','rur.')) or op == 'rsil': return [],[(a[0],1)]
    if op.startswith('wur.'): return [(a[0],1)],[]
    if op == 's32c1i': return [(a[0],1),(a[1],1)],[(a[0],2)]
    return base.operands(ins)


def lanes(items):
    return [(lane,s) for r,s in items for lane in
            ([r+'.'+str(j) for j in range(4)] if re.fullmatch(r'q[0-7]',r) else [r])]


def replay(code):
    ready, time, stalls, redirects = {}, -1, 0, 0
    for j,(address,ins) in enumerate(code):
        uses,defs = operands(ins)
        issue = time+1
        for r,stage in lanes(uses):
            if r in ready:
                at,defined = ready[r]
                issue = max(issue,at+max(defined-stage+1,0))
        stalls += issue-time-1
        for r,stage in lanes(defs): ready[r] = issue,stage
        op = ins.split()[0]
        next_pc = code[j+1][0] if j+1 < len(code) else address+(2 if op.endswith('.n') else 3)
        taken = op.startswith(('b','j','call','ret','rf')) and next_pc != address+(2 if op.endswith('.n') else 3)
        redirects += taken
        time = issue+(2 if taken else 0)
        if op.startswith(('quou','quos','remu')): time = max(time,issue+10)
    return dict(cycles=time+1,instructions=len(code),interlock_cycles=stalls,redirects=redirects)


def traces(path,disassembly):
    f = base.functions(disassembly)
    begin,end = [f[x][0][0] for x in ('lofty_begin','lofty_end')]
    blocks,current,active,rows = {},[],None,[]
    for line in pathlib.Path(path).open():
        m = re.match(r'^0x([0-9a-f]+):\s+(.*)',line)
        if m: current.append((int(m[1],16),m[2])); continue
        if current: blocks[current[0][0]] = current; current = []
        event = re.match(r'^Trace 0: .*?\[[^/]+/([0-9a-f]+)/',line)
        if not event: continue
        pc = int(event[1],16)
        if pc == end:
            assert active is not None
            rows.append(active); active = None
        if pc == begin:
            assert active is None
            active = []
        if active is not None: active.extend(blocks[pc])
    assert len(rows) == 49,len(rows)
    return rows


def report(trace,disassembly):
    rows = traces(trace,disassembly)
    for j,mc in enumerate((13,15,17,11)):
        frames = []
        for frame in range(2):
            stages = {name:replay(row) for name,row in zip(('bb','bch','ldpc','map','first_page','frame'),
                      rows[12*j+6*frame:12*j+6*frame+6])}
            kernel = sum(r['cycles'] for r in stages.values())
            frames.append(kernel)
            duration = 33282*7.2 if mc == 11 else 199746
            core0 = math.ceil((kernel+4823)*47220/duration)+1000
            yield dict(kind='encoder_model',modcod=mc,frame=frame,stages=stages,kernel_cycles=kernel,
                       input_copy_planning_floor=4823,total_cycles=kernel+4823,
                       sensitivity_25_percent=math.ceil(1.25*kernel)+4823,
                       frame_cycles=duration,ceiling_cycles=193848 if mc == 11 else 160000,
                       core0_bank_cycles=core0,publication_wake_allowance_per_bank=1000,
                       radio_off_reserve_15_percent=core0 <= 40137,
                       keyed_tx_planning_cycles=46000 if mc == 11 else 38344,
                       keyed_reserve_target_met=kernel+4823 <= (157685 if mc == 11 else 132000),
                       first_publication_cycles=kernel-stages['frame']['cycles']+4823,measured=False)
        yield dict(kind='encoder_pair',modcod=mc,mean_cycles=sum(frames)/2+4823,
                   max_cycles=max(frames)+4823,measured=False)
    r = replay(rows[-1])
    yield dict(kind='render_trace',blocks=1365,samples=16380,**r,measured=False)
    f = base.functions(disassembly)['lofty_render_pie']
    begin = next(i for i,(_,ins) in enumerate(f) if re.match(r'extui\s+a10, a8, 0, 13$',ins))
    end = next(i for i,(_,ins) in enumerate(f) if re.match(r'loopnez\s+a14,',ins))
    while f[end-1][1].startswith('nop'): end -= 1 # assembler padding after LEND
    group = replay(f[begin:end])['cycles']
    assert len(f[begin:end]) >= 46 # admission uses at least 23 issues per block
    step = group/2
    # The executed 1365-block trace includes 682 two-block groups and one
    # single-block tail. Calls, loop setup and memory stalls are separate.
    prologue = r['cycles']-682*group-24
    assert group == 47 and 0 <= prologue < 256,(group,prologue)
    yield dict(kind='render_model',cycles_per_block=step,cycles_per_sample=step/12,
               prologue_cycles=prologue,previous_cycles_per_block=24.5,
               minimum_page_cycles=23*128,
               issue_saving_per_15740_sample_bank=math.ceil(15740/12)*(24.5-step),
               ceiling_cycles=40137,deadline_cycles=47220,measured=False)



if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace',type=pathlib.Path)
    p.add_argument('disassembly',type=pathlib.Path)
    a = p.parse_args()
    for row in report(a.trace,a.disassembly): print(json.dumps(row,sort_keys=True))
