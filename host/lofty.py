#!/usr/bin/env python3
"""Independent five-symbol 8PSK convolution. No device access."""
import functools
import numpy as np



# EN 302 307-1 Annex A: .35 template, frequency in Rs/2, amplitude in dB.
LEVEL_I = np.rint(np.cos(np.arange(8)*np.pi/4)*2**30).astype(np.int64)
LEVEL_Q = np.rint(np.sin(np.arange(8)*np.pi/4)*2**30).astype(np.int64)
MASK = {.35: dict(upper=[(0,.25),(.2,.25),(.4,.25),(.8,.15),(.9,-.5),(1,-2),
                         (1.2,-8),(1.4,-16),(1.6,-24),(1.8,-35),(2.12,-40)],
                 lower=[(0,-.25),(.2,-.4),(.4,-.4),(.8,-1.1),(1,-4),(1.2,-11)])}


def iq(words):
    w = np.asarray(words,np.int64)
    signed = lambda v: ((v&1023)^512)-512
    return signed(w)+1j*signed(w>>10)


def corrected_rrc(alpha, n, span=5, beta=1.5):
    """Window the inverse transform of RRC(f)/sinc(f/Fs), then truncate to span symbols.

    f is in Rs, Fs = n Rs. Integrate only the ideal RRC band, where sinc is nonzero.
    DC correction is 1; as n tends to infinity this tends to window_rrc(). The hold's
    linear phase is a delay, handled by receiver timing, not by this amplitude correction.
    EN 302 307-1 V1.4.1 5.6 p35 defines the ideal RRC response, not this finite FIR.
    """
    f = np.linspace(0, (1 + alpha) / 2, 16385)
    t = (np.arange(span * n) - (span * n - 1) / 2) / n
    a = rrc_response(f, alpha) / np.sinc(f / n)
    g = np.concatenate([2 * np.trapezoid(a * np.cos(2 * np.pi * np.outer(ti, f)), f, axis=1)
                        for ti in np.array_split(t, 1 + len(t)//64)])
    return g * np.kaiser(span * n, beta)

def response_db(taps, n, fn, hold=True):
    """20 log10 |G(f) / G(0)| of a symmetric FIR at f = fn * Rs / 2, times the DAC's zero-order
    hold sinc(f / Fs) if hold."""
    k = np.arange(len(taps)) - (len(taps) - 1) / 2
    g, fn = np.asarray(taps, float), np.atleast_1d(np.asarray(fn, float))
    h = np.concatenate([(np.cos(np.outer(f, k) * np.pi / n) * g).sum(axis=1)
                        for f in np.array_split(fn, 1 + len(fn) // 4096)])
    if hold:
        h = h * np.sinc(fn / (2 * n))
    return 20 * np.log10(np.abs(h) / abs(g.sum()) + 1e-300)

def mask_margins(taps, n, alpha, hold=True, step=1e-4):
    """Smallest distance (dB, positive = inside) of the pulse response to the Annex A template:
    upper limit up to S, lower limit up to M, and the -40 dB floor from S to Fs / 2. Grid of
    step * fN plus every template point."""
    up, lo = (np.array(MASK[alpha][k], float) for k in ("upper", "lower"))
    fn = np.unique(np.r_[np.arange(0, n + step / 2, step), up[:, 0], lo[:, 0]])
    db = response_db(taps, n, fn, hold)
    near = fn <= up[-1, 0]
    upper = np.interp(fn[near], up[:, 0], up[:, 1]) - db[near]
    m = fn <= lo[-1, 0]
    lower = db[m] - np.interp(fn[m], lo[:, 0], lo[:, 1])
    far = -40 - db[~near]
    return dict(upper=upper.min(), upper_at=fn[near][upper.argmin()], lower=lower.min(),
                lower_at=fn[m][lower.argmin()], far=far.min(), far_at=fn[~near][far.argmin()])

def rrc_response(f, alpha):
    """Ideal RRC amplitude, f in units of Rs."""
    f = np.abs(f)
    lo, hi = (1 - alpha) / 2, (1 + alpha) / 2
    h = np.where(f <= lo, 1.0, 0.0)
    m = (f > lo) & (f < hi)
    h[m] = np.sqrt(0.5 * (1 + np.cos(np.pi / alpha * (f[m] - lo))))
    return h


@functools.lru_cache(None)
def taps(peak=128):
    g = corrected_rrc(.35,3,5,1.5)
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
    levels = np.stack((LEVEL_I,LEVEL_Q))
    for part,count in enumerate((3,3,2)):
        codes = (np.arange(8**count)[:,None] >> (3*np.arange(count-1,-1,-1)))&7
        v = rounded(levels[:,codes] @ weights(part,peak))
        parts.append(v)
        bounds.append(-v.min(axis=1))
    bounds[0] += 512-sum(bounds)
    packed = [((p[0]+b[0]) | ((p[1]+b[1])<<10)).astype('<u4') for p,b in zip(parts,bounds)]
    return np.r_[packed[0][:,:4].ravel(),packed[0][:64,4:12].ravel(),packed[1].ravel(),
                 packed[2][:,4:8].ravel(),packed[2][:,8:12].ravel()]



def shape(phases, peak=128):
    phases = np.asarray(phases,np.uint8)
    n = (len(phases)+3)//4
    src = np.r_[np.zeros(4,np.uint8),phases,np.zeros((-len(phases))%4,np.uint8)]
    positions = 4*np.arange(n)[:,None]+np.arange(8)[None,:]
    codes = src[positions]
    levels = np.stack((LEVEL_I,LEVEL_Q))
    out = sum(rounded(levels[:,codes[:,3*j:min(3*j+3,8)]] @ weights(j,peak)) for j in range(3))
    return ((out[0]&1023) | ((out[1]&1023)<<10)).astype('<u4').ravel()[:3*len(phases)]


def quantized_mask(peak=128):
    """Exact iid-8PSK covariance, including all three integer roundings and DAC hold."""
    groups = [tuple(range(-4,-1)),tuple(range(-1,2)),(2,3)]
    values = []
    levels = np.stack((LEVEL_I,LEVEL_Q))
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
    upper,lower = [np.array(MASK[.35][k]) for k in ('upper','lower')]
    f = np.unique(np.r_[np.arange(0,3.00005,.0001),upper[:,0],lower[:,0]])
    power = r[0].real+2*np.real(np.sum(np.exp(1j*np.pi*np.outer(f,np.arange(1,24))/3)*r[1:],axis=1))
    db = 10*np.log10(power/power[0])+20*np.log10(np.sinc(f/6))
    return dict(upper=float((np.interp(f,upper[:,0],upper[:,1])-db).min()),
                lower=float((db[f <= 1.2]-np.interp(f[f <= 1.2],lower[:,0],lower[:,1])).min()),
                far=float((-40-db[f > 2.12]).min()))
