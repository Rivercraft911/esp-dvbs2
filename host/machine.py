#!/usr/bin/env python3
"""Independent full-axis convolution and sign-window oracle. No hardware access."""
import re
import pathlib
import functools

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
GROUPS = tuple(range(-6,5))


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
    return np.trunc(np.array([int(s) for s in re.findall(r'-?\d+',text)])*((peak-1)/508)).astype(np.int64)


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
    for newest in (1,2,4):
        c = (np.arange(256)[:,None] >> np.arange(7,-1,-1)) & 1
        # One axis, eight chronological signs; independent full sum then round.
        v = (1-2*c)*759250125 @ weights(range(newest-7,newest+1),peak)[:,4*len(parts):4*len(parts)+4]
        parts.append(np.sign(v)*((np.abs(v)+(1 << 37)) >> 38))
    t = np.array(parts)&1023
    return np.stack((t,t<<10)).astype('<u4').ravel()


def descriptors(src, phase, n):
    c = codes(src)
    positions = phase+5*np.arange(n)[:,None]+np.arange(11)[None,:]
    c = c[positions]
    i, q = (c^(c>>1))&1, c>>1
    shifts = np.arange(14,3,-1)
    return (np.sum(i << shifts,axis=1) | np.sum(q << (shifts+16),axis=1)).astype('<u4')


def shape(src, phase, n, peak=128):
    c = codes(src)
    positions = 6+phase+5*np.arange(n)[:,None]+np.array(GROUPS)[None,:]
    result = contributions(c[positions],GROUPS,peak)
    assert abs(result).max(initial=0) <= peak
    return ((result[0]&1023) | (result[1]&1023)<<10).astype('<u4').ravel()
