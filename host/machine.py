#!/usr/bin/env python3
"""Independent row-program oracle. No hardware access."""
import re
import pathlib
import functools

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
GROUPS = ((-6,-5,-4), (-3,-2,-1,0), (1,2,3,4))


def checksum(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b)*16777619) & 0xffffffff
    return h


def source(seed=2026):
    out, x = bytearray(), seed
    for _ in range(1720):
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        out.append((x >> 16) & 255)
    return bytes(out)


def codes(src):
    return ((np.frombuffer(src,np.uint8)[:,None] >> np.array([6,4,2,0])) & 3).ravel()


def taps(peak):
    text = (ROOT/'firmware/main/machine.c').read_text().split('pulse[84] = {')[1].split('}')[0]
    # Same fixed pulse data; convolution, table formation and sign logic below
    # are independent of firmware. No floating sums or per-symbol rounding.
    return np.trunc(np.array([int(s) for s in re.findall(r'-?\d+',text)])*(peak-2)/508).astype(np.int64)


def weights(group, peak):
    at = 5*np.arange(12)[None,:]-12*np.array(group)[:,None]
    return np.where((at >= 0)&(at < 84),taps(peak)[np.clip(at,0,83)],0)


def contributions(symbols, group, peak):
    # QPSK signs in phase order 1,3,5,7, in 2^-30 units.
    levels = np.array([[1,-1,-1,1],[1,1,-1,-1]],np.int64)*759250125
    v = levels[:,symbols] @ weights(group,peak)
    return np.sign(v)*((np.abs(v)+(1 << 37)) >> 38)


@functools.lru_cache(None)
def table(peak=128):
    parts = []
    for group in GROUPS:
        words = np.arange(4**len(group))
        c = (words[:,None] >> (2*np.arange(len(group)-1,-1,-1))) & 3
        parts.append(contributions(c,group,peak))
    bias = np.array([-p.min(axis=1) for p in parts])
    bias[0] += 512-bias.sum(axis=0)
    fields = [p+b[:,None,:] for p,b in zip(parts,bias)]
    assert all((f >= 0).all() for f in fields)
    assert (sum(f.max(axis=1) for f in fields) < 1024).all()
    return np.concatenate([(f[0] | f[1]<<10).astype('<u4').ravel() for f in fields])


def descriptors(src, phase, n):
    c = codes(src)
    rows = []
    for group in GROUPS:
        positions = 6+phase+5*np.arange(n)[:,None]+np.array(group)[None,:]
        row = np.sum(c[positions] << (2*np.arange(len(group)-1,-1,-1)),axis=1)
        rows.append(48*row)
    return np.array(rows,dtype='<u2').T.copy()


def shape(src, phase, n, peak=128):
    c, result = codes(src), np.zeros((2,n,12),np.int64)
    for group in GROUPS:
        positions = 6+phase+5*np.arange(n)[:,None]+np.array(group)[None,:]
        result += contributions(c[positions],group,peak)
    assert abs(result).max(initial=0) <= peak
    return ((result[0]&1023) | (result[1]&1023)<<10).astype('<u4').ravel()

