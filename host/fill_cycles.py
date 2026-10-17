#!/usr/bin/env python3
"""Bank-fill issue model, no devices. Fixed costs are assumptions, not timing results."""
import argparse
import json
import math

import shaper_cycles as c

BANK, STEP, LEAD, CACHE = 16380, 14268, 64, 2176


def pieces(first, count):
    head = min(count, BANK - first % BANK)
    return [(first, head)] + ([(first + head, count - head)] if head < count else [])


def geometry(bank):
    start = bank * STEP - LEAD if bank else 0
    previous_end = (bank - 1) * STEP - LEAD + BANK if bank > 1 else BANK if bank else 0
    fresh = [(max(at, previous_end), at + n - max(at, previous_end))
             for at, n in pieces(start, BANK) if at + n > previous_end]
    return dict(start=start, fresh=fresh, reused=BANK - sum(n for _, n in fresh),
                saved=sum(min(n, CACHE) for _, n in fresh))


def coefficients(code):
    values = [c.replay(c.loop_stream(code, g))['cycles'] for g in range(3)]
    return values[0], values[1], values[2] - values[1]


def kernel(costs, symbols):
    if not symbols:
        return 0, 0
    groups, tail = divmod(symbols - 1, 6)
    one, first, step = costs
    return (first + (groups - 1) * step if groups else one), tail


def model(costs, sps, bank):
    g = geometry(bank)
    one = costs[0]
    shape, unpack = 0, 0
    for at, count in g['fresh']:
        # shaper_samples_codes_profile: partial head, whole symbols, partial tail.
        skip = at % sps
        head = min(sps - skip, count) if skip else 0
        whole, tail = divmod(count - head, sps)
        if head:
            shape += one + 32  # assumed scratch copy/wrapper cost
        if whole:
            bulk, remaining = kernel(costs, whole)
            # n4 tails use PIE calls; n8 tails still use C, assumed 100 cycles/symbol.
            shape += bulk + remaining * (one if sps == 4 else 100)
        if tail:
            shape += one + 32
        # Nine unpack instructions; allow two cycles of load/interlock overhead per 32 phases.
        # 160 cycles per fresh callback allow scalar alignment heads/tails and vector setup.
        unpack += math.ceil((skip + count) / sps / 32) * 11 + 160
    copy = (g['reused'] + g['saved']) * 0.5  # four instructions per eight aligned words
    control, wake = 1000, 2348  # assumed glue; measured old notification maximum
    total = shape + unpack + copy + control
    return dict(bank=bank, sps=sps, shaped_words=sum(n for _, n in g['fresh']),
                reused_words=g['reused'], saved_words=g['saved'], kernel_cycles=shape,
                unpack_cycles=unpack, copy_cycles=copy, assumed_control_cycles=control,
                cycles=total, with_notification_cycles=total + wake,
                stress_25pct_cycles=total * 1.25 + wake)


def report(path):
    functions = c.functions(path)
    for sps in (4, 8):
        for name in (f'shaper_fill{sps}_pie', f'shaper_fill{sps}_raw_pie'):
            groups = ((BANK // sps) - 1) // 6
            samples = (6 * groups + 1) * sps
            yield dict(kind='kernel', kernel=name, samples=samples,
                       **c.replay(c.loop_stream(functions[name], groups)))
        costs = coefficients(functions[f'shaper_fill{sps}_pie'])
        for name, banks in (('startup', [0]), ('steady', range(2, BANK // math.gcd(BANK, STEP) + 2))):
            rows = [model(costs, sps, b) for b in banks]
            yield dict(kind=name, **max(rows, key=lambda r: r['cycles']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disassembly', help='objdump -d --no-show-raw-insn main/libmain.a')
    for row in report(parser.parse_args().disassembly):
        print(json.dumps(row, sort_keys=True))
