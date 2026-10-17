"""Round-two pulse, table and work models. Host only, never opens a device."""
import argparse
import json
import math
import pathlib

import numpy as np
import shaper as s
import shaper_cycles as cycles
import maxrate

PULSE4 = np.array([-1785,138,3694,5601,2264,-6452,-15014,-14236,2790,34634,70116,93505,
                   93505,70116,34634,2790,-14236,-15014,-6452,2264,5601,3694,138,-1785])
FRAME, STEP, BANK = 22194, 15740, 16380


def quarter_tables():
    """C4 symmetry: 18 KiB signed I16/Q16 rows; exact for all three original parts."""
    idx = np.arange(128)
    representative = (idx & 1) | ((idx >> 1 & 7) << 3) | ((idx >> 4 & 7) << 6)
    full = np.moveaxis(maxrate.parts(), 0, -1)
    return full[:,representative].astype(np.int16)


def unfold_quarters(table):
    w = np.arange(512)
    phases = (w[:,None] >> (3*np.arange(3))) & 7
    r = phases[:,0] // 2
    normalized = (phases - 2*r[:,None]) & 7
    idx = normalized[:,0] | normalized[:,1] << 1 | normalized[:,2] << 4
    x = table[:,idx].astype(np.int64)
    # Four exact rotations, no floating point and no re-rounding.
    rotations = (x, np.stack((-x[...,1],x[...,0]),axis=-1), -x,
                 np.stack((x[...,1],-x[...,0]),axis=-1))
    return sum(rotations[k] * (r == k)[None,:,None,None] for k in range(4))


def profile(peak=128):
    p = s.Profile('round2', .35, 4, '8psk', span=6)
    p.__dict__['taps'] = np.trunc(PULSE4 * peak / 718.4).astype('int64')
    return p


def model(path, nsym=4095):
    """Replay actual scalar control flow, with scalar values for loop/tail counts."""
    code = cycles.functions(path)['shaper_short4_pie']
    addresses = {a:i for i,(a,_) in enumerate(code)}
    long_loop = next((j for j,(_,x) in enumerate(code) if x.startswith('wsr.lbeg')), None)
    long_end = None
    if long_loop is not None:
        branch = next(x for _,x in reversed(code[:long_loop]) if x.startswith('beqz'))
        long_end = addresses[int(branch.split(',')[-1],16)]
    regs = dict(a4=nsym)
    i, loop, trace = 0, None, []
    while True:
        _, ins = code[i]
        op, *tail = ins.split(None, 1)
        op = op.removesuffix('.n')
        a = [x.strip() for x in tail[0].split(',')] if tail else []
        taken = op == 'j'
        if op in ('beqz', 'bnez'): taken = (regs[a[0]] == 0) == (op == 'beqz')
        if op == 'blti': taken = regs[a[0]] < int(a[1])
        if op == 'bgei': taken = regs[a[0]] >= int(a[1])
        if op == 'movi': regs[a[0]] = int(a[1],0)
        elif op == 'or' and a[0]=='a14': regs[a[0]] = regs[a[1]] | regs[a[2]]
        elif op == 'mov': regs[a[0]] = regs.get(a[1],0)
        elif op == 'addx2' and a[0]=='a9': regs[a[0]] = 3*regs.get(a[1],0)
        elif op == 'slli' and a[0]=='a9': regs[a[0]] = regs.get(a[1],0)<<int(a[2])
        elif op == 'sub' and a[0]=='a14': regs[a[0]] = regs[a[1]]-regs[a[2]]
        elif op == 'remu': regs[a[0]] = regs[a[1]] % regs[a[2]]
        elif op == 'quou': regs[a[0]] = regs[a[1]] // regs[a[2]]
        elif op == 'addi' and a[0] in ('a4','a14'): regs[a[0]] = regs[a[1]] + int(a[2])
        elif op == 'loopnez':
            taken = regs[a[0]] == 0
            loop = None if taken else ((addresses[int(a[1],16)], long_end, regs[a[0]])
                                       if long_end is not None else
                                       (i+1, addresses[int(a[1],16)], regs[a[0]]))
        # The integer divide startup is accounted separately below, conservatively.
        model_ins = ins.replace('addx2', 'addx4').replace('or\t', 'and\t')
        if op in ('j','remu','quou'): model_ins = 'nop'
        if op == 'mov': model_ins = f'addi {a[0]}, {a[1]}, 0'
        trace.append((model_ins,taken))
        if op == 'retw': break
        i = addresses[int(a[-1],16)] if taken else i+1
        if loop and i == loop[1]:
            begin,end,left = loop
            loop = (begin,end,left-1) if left > 1 else None
            if loop: i = begin
    r = cycles.replay(trace)
    # 32 cycles for the divide: deliberately pessimistic startup allowance.
    # Steady loop timing is independent of this allowance, not a divider latency claim.
    r['cycles'] += 32 if nsym else 0
    return dict(kernel='shaper_short4_pie', symbols=nsym, samples=nsym*4,
                division_allowance=32 if nsym else 0, **r)


def bank_models(path):
    """Every physical bank offset, including startup. Glue remains an assumption."""
    small = [0] + [model(path,n)['cycles'] for n in range(1,25)]
    overhead = model(path,25)['cycles'] - small[1] - 193
    def kernel(n):
        if not n: return 0
        groups, tail = divmod(n-1,24)
        return small[tail+1] + groups*193 + (overhead if groups else 0)
    assert kernel(4095) == model(path,4095)['cycles']
    for bank in range(BANK // math.gcd(BANK,STEP) + 2):
        first = bank*STEP-128 if bank else 0
        previous = (bank-1)*STEP-128+BANK if bank > 1 else BANK if bank else 0
        offset = first % BANK
        pieces = [(first,BANK-offset)] + ([(first+BANK-offset,offset)] if offset else [])
        shape = unpack = saved = fresh = 0
        for at, count in pieces:
            count = at+count-max(at,previous)
            if count <= 0: continue
            assert count % 4 == 0
            shape += kernel(count//4)
            unpack += math.ceil(count/4/32)*11 + 160
            keep = max(0,768-offset) if at == first and offset else 768
            saved += min(count,keep)
            fresh += count
        copies = (BANK-fresh+saved)*.5
        wake, tick, control = 2396 if bank else 0, 1068, 1000
        yield dict(bank=bank, kernel_cycles=shape, unpack_cycles=unpack,
                   saved_words=saved, reused_words=BANK-fresh, copy_cycles=copies,
                   notification_cycles=wake, tick_cycles=tick, assumed_control_cycles=control,
                   cycles=shape+unpack+copies+wake+tick+control)


def budget(fill, encoder, sps):
    period = FRAME * sps * 3
    work = fill * FRAME*sps/STEP + encoder
    return dict(sps=sps, fill_cycles=fill, encoder_cycles=encoder,
                bank_cycles=STEP*3, frame_cycles=period,
                total_cycles=work, combined_margin=1-work/(2*period),
                fill_margin=1-fill/(STEP*3), encoder_margin=1-encoder/period)


def report(disassembly=None):
    yield dict(kind='symmetry', evidence='exhaustive host identity',
               full_bytes=73728, quarter_bytes=quarter_tables().nbytes,
               restored_exact=bool(np.array_equal(unfold_quarters(quarter_tables()),
                                                  np.moveaxis(maxrate.parts(),0,-1))))
    yield dict(kind='quality', evidence='linear host model, ideal DAC hold',
               mask=s.mask_margins(profile().taps,4,.35),
               pulse_mer_db=s.pulse_mer(profile()), table_bytes=16384)
    if disassembly:
        yield dict(kind='instructions', evidence='issue model, not silicon', **model(disassembly))
        rows = list(bank_models(disassembly))
        yield dict(kind='startup_fill', evidence='mixed model, not silicon', **rows[0])
        worst = max(rows[1:],key=lambda r:r['cycles'])
        yield dict(kind='steady_fill', evidence='mixed model, not silicon', **worst)
        # Prior measured worst minus the old fill issue model, tick included:
        # 43,791 - (41,366 + 1,068) = 1,357. Carry the whole residual forward.
        yield dict(kind='calibrated_fill', evidence='sensitivity model, not a bound',
                   prior_residual_cycles=1357, cycles=worst['cycles']+1357,
                   margin=1-(worst['cycles']+1357)/47220)
    # Explicit sensitivity range. Complete fills, rather than isolated kernel means, decide.
    for fill in (39000,40137):
        for enc in (167000,185000):
            yield dict(kind='20m_budget', evidence='conditional model', **budget(fill,enc,4))
    for enc in (167000,133500):
        for fill in (46443,49000):
            yield dict(kind='26m_budget', evidence='conditional model; 72 KiB layout not integrated',
                       **budget(fill,enc,3))

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--disassembly', type=pathlib.Path)
    a = ap.parse_args()
    for r in report(a.disassembly): print(json.dumps(r,sort_keys=True))
