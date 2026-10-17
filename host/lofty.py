#!/usr/bin/env python3
"""Independent five-symbol 8PSK convolution. No device access."""
import functools
import numpy as np
import shaper


@functools.lru_cache(None)
def taps(peak=128):
    g = shaper.corrected_rrc(.35,3,5,1.5)
    fixed = np.rint(g*508*256/np.abs(g.reshape(5,3)).sum(axis=0).max()).astype(np.int64)
    return np.trunc(fixed*((peak-2)/508)).astype(np.int64)


def weights(part, peak):
    positions = np.arange(-4+3*part,min(-1+3*part,4))
    at = np.arange(12)[None,:]-3*positions[:,None]
    return np.where((at >= 0)&(at < 15),taps(peak)[np.clip(at,0,14)],0)


def rounded(x):
    return np.sign(x)*((np.abs(x)+(1<<37))>>38)


@functools.lru_cache(None)
def table(peak=128):
    parts, bounds = [], []
    levels = np.stack((shaper.LEVEL_I,shaper.LEVEL_Q))
    for part,count in enumerate((3,3,2)):
        codes = (np.arange(8**count)[:,None] >> (3*np.arange(count-1,-1,-1)))&7
        v = rounded(levels[:,codes] @ weights(part,peak))
        parts.append(v)
        bounds.append(-v.min(axis=1))
    bounds[0] += 512-sum(bounds)
    packed = [((p[0]+b[0]) | ((p[1]+b[1])<<10)).astype('<u4') for p,b in zip(parts,bounds)]
    return np.r_[packed[0].ravel(),packed[1].ravel(),np.repeat(packed[2][:,4:8],2,axis=0).ravel(),
                 np.repeat(packed[2][:,8:12],2,axis=0).ravel()]


def shape(phases, peak=128):
    phases = np.asarray(phases,np.uint8)
    n = (len(phases)+3)//4
    src = np.r_[np.zeros(4,np.uint8),phases,np.zeros((-len(phases))%4,np.uint8)]
    positions = 4*np.arange(n)[:,None]+np.arange(8)[None,:]
    codes = src[positions]
    levels = np.stack((shaper.LEVEL_I,shaper.LEVEL_Q))
    out = sum(rounded(levels[:,codes[:,3*j:min(3*j+3,8)]] @ weights(j,peak)) for j in range(3))
    return ((out[0]&1023) | ((out[1]&1023)<<10)).astype('<u4').ravel()[:3*len(phases)]


def quantized_mask(peak=128):
    """Exact iid-8PSK covariance, including all three integer roundings and DAC hold."""
    groups = [tuple(range(-4,-1)),tuple(range(-1,2)),(2,3)]
    values = []
    levels = np.stack((shaper.LEVEL_I,shaper.LEVEL_Q))
    for j,g in enumerate(groups):
        codes = (np.arange(8**len(g))[:,None] >> (3*np.arange(len(g)-1,-1,-1)))&7
        v = rounded(levels[:,codes] @ weights(j,peak))
        values.append((v[0]+1j*v[1]).reshape((8,)*len(g)+(12,)))
    cov = []
    for distance in (0,4):
        c = np.zeros((12,12),complex)
        for g,x in zip(groups,values):
            for h,y in zip(groups,values):
                shifted = tuple(p+distance for p in h)
                shared = set(g)&set(shifted)
                if not shared: continue
                a = x.mean(axis=tuple(j for j,p in enumerate(g) if p not in shared)).reshape(-1,12)
                b = y.mean(axis=tuple(j for j,p in enumerate(shifted) if p not in shared)).reshape(-1,12)
                c += np.einsum('ip,iq->pq',a,b.conj())/len(a)
        cov.append(c)
    r = np.array([sum(cov[(p+lag)//12][p,(p+lag)%12] for p in range(12)
                      if p+lag < 24)/12 for lag in range(24)])
    upper,lower = [np.array(shaper.MASK[.35][k]) for k in ('upper','lower')]
    f = np.unique(np.r_[np.arange(0,3.00005,.0001),upper[:,0],lower[:,0]])
    power = r[0].real+2*np.real(np.sum(np.exp(1j*np.pi*np.outer(f,np.arange(1,24))/3)*r[1:],axis=1))
    db = 10*np.log10(power/power[0])+20*np.log10(np.sinc(f/6))
    return dict(upper=float((np.interp(f,upper[:,0],upper[:,1])-db).min()),
                lower=float((db[f <= 1.2]-np.interp(f[f <= 1.2],lower[:,0],lower[:,1])).min()),
                far=float((-40-db[f > 2.12]).min()))
