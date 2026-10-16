#!/usr/bin/env python3
"""Steady-loop model for objdump -d of the APSK PIE kernels, not a measurement.

Replay the compiled instructions with the fec-limit method: TRM v1.8 section
1.7.1 pp65-66 and table 1.7-2 pp66-73. ALU operands use/define stage E=1,
loads define M=2, single issue, taken branches cost two extra cycles (1.7.3).
No SRAM bank conflicts, DAC contention, cache misses, interrupts or entry cost.
"""
import json
import pathlib
import re
import sys


def operands(text):
    """Return (uses, defs) with (register, pipeline stage) pairs. Reject unknown ops."""
    op,*tail=text.split(None,1)
    op=op.removesuffix('.n')
    args=[x.strip() for x in tail[0].split(',')] if tail else []
    if op=='ee.vld.128.ip':
        return [(args[1],1)],[(args[0],2),(args[1],1)]
    if op=='ee.vst.128.ip':
        return [(args[0],1),(args[1],1)],[(args[1],1)]
    if op=='ee.vadds.s32.ld.incp':
        q,base,d,x,y=args
        return [(base,1),(x,1),(y,1)],[(q,2),(base,1),(d,1)]
    if op=='ee.vadds.s32.st.incp':
        q,base,d,x,y=args
        return [(q,1),(base,1),(x,1),(y,1)],[(base,1),(d,1)]
    if op in ['ee.vadds.s32','ee.xorq','ee.andq','ee.orq']:
        return [(args[1],1),(args[2],1)],[(args[0],1)]
    if op=='ee.vsr.32':
        return [(args[1],1),('SAR',1)],[(args[0],1)]
    if op=='ssai':
        return [],[('SAR',1)]
    if op=='l8ui':
        return [(args[1],1)],[(args[0],2)]
    if op in ['add','addi','slli','or','extui']:
        return [(r,1) for r in args[1:] if re.fullmatch('a[0-9]+',r)],[(args[0],1)]
    if op=='bnez':
        return [(args[0],1)],[]
    if op=='nop':
        return [],[]
    raise ValueError('unmodelled instruction: '+text)


def cycles(body,iterations=8):
    ready={};time=-1;starts=[]
    for _ in range(iterations):
        starts.append(time+1)
        for ins in body:
            use,defs=operands(ins)
            issue=time+1
            for r,stage in use:
                if r in ready:
                    at,defined=ready[r]
                    issue=max(issue,at+max(defined-stage+1,0))
            for r,stage in defs: ready[r]=(issue,stage)
            time=issue+(2 if ins.startswith('bnez') else 0)
    return (starts[-1]-starts[-3])/2


def report(path):
    functions={};current=None
    for line in pathlib.Path(path).read_text().splitlines():
        label=re.match(r'^([0-9a-f]+) <(apsk_(?:single|pair)_[^>]+)>:',line)
        if label:
            current=label[2];functions[current]=[]
        elif re.match(r'^[0-9a-f]+ <',line): current=None
        ins=re.match(r'^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(.+)$',line)
        if ins and current:
            functions[current].append((int(ins[1],16),ins[2].split('<')[0].strip()))
    for name,ins in functions.items():
        match=re.search('_n([48])_f[03]$',name)
        if not match: continue
        back=[(at,op) for at,op in ins if op.startswith('bnez') and int(op.split(',')[1].strip(),16)<at]
        if len(back)!=1: raise ValueError('expected one steady loop: '+name)
        end,branch=back[0];start=int(branch.split(',')[1].strip(),16)
        body=[op for at,op in ins if start<=at<=end]
        symbols=9 if 'single' in name else 7
        n=int(match[1]);cp=cycles(body)/(symbols*n)
        yield dict(kernel=name,loop_instructions=len(body),loop_cycles=cycles(body),
                   symbols_per_loop=symbols,cycles_per_sample=cp,
                   core_40_percent=100*cp/6,core_80_percent=100*cp/3,
                   bank_40_percent=100*cp/6*16380/14268,
                   bank_80_percent=100*cp/3*16380/14268)


if __name__=='__main__':
    for row in report(sys.argv[1]): print(json.dumps(row,sort_keys=True))
