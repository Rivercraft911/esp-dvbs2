"""Four-symbol n3 table experiment and whole-transmitter budgets. No hardware access.

python3 host/maxrate.py [--disassembly notes/block.dis]
The packed input is four consecutive 8PSK phases in 12 bits, newest first.
Only the block's first output sample needs five prior symbols. Its last needs
the next three, so nine input symbols split into three independent triples.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib

import numpy as np
import shaper as s
import shaper_cycles

CPU, FS, FRAME, STEP = 240_000_000, 80_000_000, 22194, 14268
ENCODER = 262100  # measured mean, results/2026-10-16-next/pace-nibble.jsonl
PULSE = np.array([-1684,1817,5606,1249,-11208,-15587,6665,51998,90010,
                  90010,51998,6665,-15587,-11208,1249,5606,1817,-1684])


def taps():
    """Same underlying pulse as rd-short; reserve 3 LSB for three roundings."""
    return np.trunc(PULSE * (125 / (np.abs(PULSE.reshape(6, 3)).sum(0).max()/256))).astype('int64')


def parts():
    w = np.arange(512)
    codes = (w[:, None] >> (3*np.arange(3))) & 7
    out = np.zeros((2, 3, 512, 12), np.int64)
    for axis, level in enumerate((s.LEVEL_I, s.LEVEL_Q)):
        for part in range(3):
            for p in range(12):
                ages = p//3 - (3*part-3-np.arange(3))
                valid = (ages >= 0) & (ages < 6)
                total = (level[codes[:, valid]] * taps()[3*ages[valid]+p%3]).sum(1)
                out[axis, part, :, p] = np.sign(total) * ((abs(total)+(1 << 37)) >> 38)
    return out


def tables():
    r = parts()
    bias = -r.min(2)
    bias[:, 1] = 512 - bias[:, 0] - bias[:, 2]
    fields = r + bias[:, :, None, :]
    assert fields.min() >= 0 and fields.max(2).sum(1).max() <= 1023
    return (fields[0] | fields[1] << 10).astype(np.uint32)


def pack(phases):
    """Four phases, newest in low bits. Caller pads frames as one continuous stream."""
    phases = np.asarray(phases, np.uint16)
    if len(phases) % 4 or np.any(phases > 7):
        raise ValueError('need whole groups of four phases in 0..7')
    return np.sum(phases.reshape(-1, 4) << np.array([9, 6, 3, 0]), axis=1).astype('<u2')


def shape(phases):
    """Eight history phases, then a whole number of four-symbol blocks."""
    words = pack(phases)
    h = (words[:-2].astype(np.uint32) << 24) | (words[1:-1].astype(np.uint32) << 12) | words[2:]
    table = tables()
    return ((table[0, (h >> 18) & 511] + table[1, (h >> 9) & 511] +
             table[2, h & 511]) ^ s.XOR).ravel()


def budget(fill, encoder=ENCODER, fs=FS, sps=3):
    """Conservation of CPU work; moving a stage cannot change this sum."""
    period = CPU*FRAME*sps/fs
    fills = FRAME*sps/STEP
    work = encoder + fills*fill
    return dict(dac_rate_hz=fs, symbol_rate_hz=fs/sps, frame_cycles=period,
                bank_cycles=STEP*CPU/fs, fill_cycles=fill, encoder_cycles=encoder,
                fill_cycles_per_frame=fills*fill, two_core_cycles_per_frame=work,
                two_core_capacity=2*period, two_core_margin=1-work/(2*period),
                max_encoder_15pct=2*period*.85-fills*fill,
                fixed_split_margin=min(1-fill/(STEP*CPU/fs), 1-encoder/period),
                payload_bps=(43040-80)*fs/(sps*FRAME))


def cycle_model(path, groups=1365):
    """Execute the assembled straight-line/basic-block schedule, not QEMU CCOUNT."""
    code = shaper_cycles.functions(path)['shaper_block_pie']
    by_address = {a:i for i,(a,_) in enumerate(code)}
    stream, i, remaining, loop = [], 0, groups, None
    while True:
        _, ins = code[i]
        op = ins.split()[0].removesuffix('.n')
        taken = op == 'j' or (op in ('beqz', 'bnez') and (remaining == 0) == (op == 'beqz'))
        if op == 'loopnez':
            end = by_address[int(ins.split(',')[-1].strip(), 16)]
            taken = remaining == 0
            loop = None if taken else (i+1, end)
        stream.append((ins, taken))
        if op == 'retw': break
        if ins.replace('\t', ' ').startswith('addi') and 'a4, a4, -1' in ins:
            remaining -= 1
        i = by_address[int(ins.split(',')[-1].split()[-1], 16)] if taken else i+1
        if loop and i == loop[1]:
            remaining -= 1
            if remaining: i = loop[0]
            else: loop = None
    # These scalar instructions have the same E-stage definition as ADD (TRM 1.7.1).
    stream = [(ins.replace('addx2', 'addx4').replace('or\t', 'and\t'), taken)
              for ins, taken in stream]
    stream = [('nop' if ins.startswith('j') else ins, taken) for ins,taken in stream]
    return dict(kernel='shaper_block_pie', groups=groups, samples=groups*12,
                **shaper_cycles.replay(stream))


def report(path=None):
    yield dict(kind='assumptions', evidence='host model, not silicon',
               encoder_source='results/2026-10-16-next/pace-nibble.jsonl',
               overlap_words_read=2112, overlap_words_saved=2176,
               copy_cycles_per_word=.5, assumed_control_cycles=500,
               table_bytes=3*512*12*4, input_bytes_per_four_symbols=2)
    yield dict(kind='budget', candidate='rd-short', **budget(60700))
    yield dict(kind='budget', candidate='rd-short 25% guard', **budget(60700*1.25))
    if path:
        m = cycle_model(path)
        yield dict(kind='cycle_model', **m)
        fill = m['cycles']*STEP/m['samples'] + (2112+2176)*.5 + 500
        for factor in (1, 1.25):
            for fs in (FS, FS/2):
                yield dict(kind='budget', candidate='block4', model_guard=factor,
                           **budget(fill*factor, fs=fs))


def loopbacks(receiver):
    """Independent receiver, synthetic samples. Includes the starting span-6 pulse."""
    import onair
    for mc in (13, 15):
        phases = np.rint(np.angle(onair.symbols(mc, 16))*4/np.pi).astype(int) % 8
        short = s.Profile('short6', .35, 3, '8psk', span=6)
        short.__dict__['taps'] = np.trunc(PULSE * (126 / (abs(PULSE.reshape(6, 3)).sum(0).max()/256))).astype('int64')
        codes = (np.arange(512)[None, :] >> (3*np.arange(3)[:, None])) & 7
        r = [np.einsum('aw,jap->jwp', level[codes], short.taps.reshape(2, 3, 3))
             for level in (s.LEVEL_I, s.LEVEL_Q)]
        short.__dict__['parts'] = [np.sign(v)*((abs(v)+(1 << 37)) >> 38) for v in r]
        for name, words in (('short6', s.shape(short, np.r_[np.zeros(5, int), phases] << 4)),
                            ('block4', shape(np.r_[np.zeros(8, int), phases]))):
            z = s.iq(words)
            samples = np.column_stack((z.real, z.imag)).astype('<i2')
            with tempfile.TemporaryDirectory() as tmp:
                path = pathlib.Path(tmp, 'samples.iq')
                samples.tofile(path)
                command = [sys.executable, str(s.ROOT/'host/onair.py'), str(path), '--rate', str(FS),
                           '--symbol-rate', str(FS/3), '--scale', '512', '--modcod', str(mc),
                           '--stream', '16', '--receiver', str(receiver), '--mer', '--shift', '0']
                run = subprocess.run(command, capture_output=True, text=True, check=True)
                row = json.loads(run.stdout)
            assert row['payload_match'] and row['payload_frames'] >= 10, row
            assert row['ldpc_bit_flips'] == 0 and row['ldpc_checks_before'] == 0, row
            assert row['bch_failed_frames'] == 0 and row['mer']['mer_db'] > 38, row
            row.pop('capture')
            yield dict(kind='host_decode', candidate=name, modcod=mc,
                       evidence='host generated; no hardware; MER excludes DAC hold',
                       crc32=zlib.crc32(samples.tobytes()), **row)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disassembly', type=pathlib.Path)
    parser.add_argument('--receiver', type=pathlib.Path, help='also run four independent LeanDVB decodes')
    args = parser.parse_args()
    for row in report(args.disassembly): print(json.dumps(row, sort_keys=True))
    if args.receiver:
        for row in loopbacks(args.receiver): print(json.dumps(row, sort_keys=True), flush=True)
