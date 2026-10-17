#!/usr/bin/env python3
"""Replay assembled shaper instructions through the fec-limit LX7/PIE issue model.

Input: xtensa-esp32s3-elf-objdump -d --no-show-raw-insn .../main/libmain.a
TRM v1.8 1.7.1 pp65-66, Table 1.7-2 pp68-69, 1.7.3 p74. Single issue,
load defines M=2, ALU defines E=1, consumers use E=1. Taken branches add 2.
No bus/cache stalls, interrupts, window spills or call/return redirects. QEMU
checks instruction semantics separately. Numbers are modelled, never QEMU timings.
"""
import argparse
import json
import pathlib
import re


def operands(ins):
    op, *tail = ins.split(None, 1)
    op = op.removesuffix('.n')
    a = [x.strip() for x in tail[0].split(',')] if tail else []
    reg = lambda r: re.fullmatch(r'[aq][0-9]+', r)
    if op == 'ee.vld.128.ip':
        return [(a[1],1)], [(a[0],2),(a[1],1)]
    if op == 'ee.vst.128.ip':
        return [(a[0],1),(a[1],1)], [(a[1],1)]
    if op == 'ee.vadds.s32.ld.incp':
        return [(a[1],1),(a[3],1),(a[4],1)], [(a[0],2),(a[1],1),(a[2],1)]
    if op == 'ee.vadds.s32.st.incp':
        return [(a[0],1),(a[1],1),(a[3],1),(a[4],1)], [(a[1],1),(a[2],1)]
    if op in ('ee.vadds.s32','ee.xorq','ee.orq','ee.andq'):
        return [(a[1],1),(a[2],1)], [(a[0],1)]
    if op == 'ee.src.q':
        return [(a[1],1),(a[2],1),('SARB',1)], [(a[0],1)]
    if op == 'ee.srcq.128.st.incp':
        return [(a[0],1),(a[1],1),(a[2],1),('SARB',1)], [(a[2],1)]
    if op == 'wur.sar_byte':
        return [(a[0],1)], [('SARB',1)]
    if op == 'ee.movi.32.q':
        return [(a[1],1)], [(a[0],1)]
    if op in ('l8ui','l16ui','l32i','l32r'):
        return ([] if op == 'l32r' else [(a[1],1)]), [(a[0],2)]
    if op in ('s32i','s8i'):
        return [(a[0],1),(a[1],1)], []
    if op in ('mov','mull','add','addi','addx2','addx4','addx8','and','or','sub','movi','extui','slli','srli'):
        return [(r,1) for r in a[1:] if reg(r)], [(a[0],1)]
    if op.startswith('b') or op in ('loop','loopnez'):
        return [(r,1) for r in a if reg(r)], []
    if op.startswith('rsr.'):
        return [], [(a[0],1)]
    if op.startswith('wsr.'):
        return [(a[0],1)], []
    if op in ('entry','retw','nop','isync'):
        return [], []
    raise ValueError('unmodelled instruction: ' + ins)


def replay(stream):
    """Stream of (instruction, taken_control_transfer) pairs; return issue cycles."""
    ready, time, stalls = {}, -1, 0
    for ins, taken in stream:
        uses, defs = operands(ins)
        issue = time + 1
        for r, stage in uses:
            if r in ready:
                at, defined = ready[r]
                issue = max(issue, at + max(defined-stage+1, 0))
        stalls += issue-time-1
        for r, stage in defs:
            ready[r] = issue, stage
        time = issue + (2 if taken else 0)
    return dict(cycles=time+1, instructions=len(stream), interlock_cycles=stalls)


def functions(path):
    out, current = {}, None
    for line in pathlib.Path(path).read_text().splitlines():
        label = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if label:
            current = label[1]
            out[current] = []
        ins = re.match(r'^\s*([0-9a-f]+):\s+(.+)$', line)
        if ins and current:
            out[current].append((int(ins[1],16), ins[2].split('<')[0].strip()))
    # .align padding before the next entry point disassembles as trailing 'ill'; never executed.
    for code in out.values():
        while code and code[-1][1] == 'ill':
            code.pop()
    return out


def loop_stream(code, groups):
    """n4, and assembler-expanded long n8 LOOPNEZ."""
    long = next((i for i,(_,s) in enumerate(code) if s.startswith('wsr.lbeg')), None)
    if long is not None:
        begin = next(i for i,(_,s) in enumerate(code) if re.match(r'addi\S*\s+a4, a4, 1$',s))+1
        end = len(code)-3
    else:
        k = next(i for i,(_,s) in enumerate(code) if s.startswith(('loopnez','loop\t')))
        begin = k+1
        end_addr = int(code[k][1].split(',')[-1].strip(),16)
        end = next(i for i,(a,_) in enumerate(code) if a == end_addr)
    return [(s, groups == 0 and s.startswith('loopnez')) for _,s in code[:begin]] + [(s,False) for _,s in code[begin:end]]*groups + [(s,False) for _,s in code[end:]]


def bank4(code):
    """Worst kernel work at any physical bank wrap, using PIE single-symbol tails.

    Call/return redirects and wrapper instructions belong to the separate control allowance.
    """
    one = replay(loop_stream(code,0))['cycles']
    first = replay(loop_stream(code,1))['cycles']
    per_group = replay(loop_stream(code,2))['cycles']-first
    base = first-per_group
    def fill(n):
        if not n:
            return 0
        groups, tail = divmod(n-1,6)
        return (base+groups*per_group if groups else one)+tail*one
    choices = [(fill(i)+fill(4095-i),i) for i in range(4096)]
    worst, split = max(choices)
    return dict(worst_bank_kernel_cycles=worst, split_symbols=split,
                single_bank_kernel_cycles=fill(4095))


def report(path):
    f = functions(path)
    for name, groups, samples in [('shaper_fill4_pie',682,16372), ('shaper_fill8_pie',341,16376)]:
        row = replay(loop_stream(f[name], groups))
        row.update(kind='cycle_model', kernel=name, groups=groups, samples=samples,
                   cycles_per_sample=row['cycles']/samples)
        if name == 'shaper_fill4_pie': row.update(bank4(f[name]))
        yield row


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('disassembly')
    args = ap.parse_args()
    for row in report(args.disassembly):
        print(json.dumps(row, sort_keys=True))
