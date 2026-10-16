#!/usr/bin/env python3
"""Independent APSK shaping prototypes. No board or RF access.

EN 302 307-1 V1.4.1, figures 11/12 and tables 9/10, pp29-30.
Input codes are the binary labels in those figures, after PL rotation. Codes
M..M+3 are unit-energy QPSK at pi/4 + k*pi/2 (headers and scrambled pilots).
The pair kernel accepts APSK data only; use the single kernel across PL changes.
"""
from __future__ import annotations
import argparse
import json
from dataclasses import dataclass
from functools import cached_property
import numpy as np
import shaper as s

GAMMA16 = {'2/3': 3.15, '3/4': 2.85, '4/5': 2.75, '5/6': 2.70, '8/9': 2.60, '9/10': 2.57}
GAMMA32 = {'3/4': (2.84, 5.27), '4/5': (2.72, 4.87), '5/6': (2.64, 4.64),
           '8/9': (2.54, 4.33), '9/10': (2.53, 4.30)}
# Labels in counterclockwise phase order. Inner/middle start at pi/4, pi/12;
# 32APSK outer starts at zero. Written from the standard's figures, not receiver code.
LABEL16 = ([12, 14, 15, 13], [4, 0, 8, 10, 2, 6, 7, 3, 11, 9, 1, 5])
LABEL32 = ([17, 21, 23, 19], [16, 0, 1, 5, 4, 20, 22, 6, 7, 3, 2, 18],
           [24, 8, 25, 9, 13, 29, 12, 28, 30, 14, 31, 15, 11, 27, 10, 26])
L, HISTORY, LEVEL_FRAC, TAP_FRAC = 9, 8, 14, 16


def constellation(m, rate):
    """Unit average energy, including four additional unit-energy QPSK points."""
    if m == 16:
        g = GAMMA16[rate]
        radii, labels, starts = [1, g], LABEL16, [np.pi/4, np.pi/12]
        norm = np.sqrt((1 + 3*g*g)/4)
    elif m == 32:
        g1, g2 = GAMMA32[rate]
        radii, labels, starts = [1, g1, g2], LABEL32, [np.pi/4, np.pi/12, 0]
        norm = np.sqrt((1 + 3*g1*g1 + 4*g2*g2)/8)
    else:
        raise ValueError('m must be 16 or 32')
    out = np.empty(m + 4, complex)
    for r, lab, start in zip(radii, labels, starts):
        out[lab] = r/norm * np.exp(1j*(start + np.arange(len(lab))*2*np.pi/len(lab)))
    out[m:] = np.exp(1j*(np.pi/4 + np.arange(4)*np.pi/2))
    return out


def rounded(x, shift):
    return (x + (1 << (shift-1))) >> shift


@dataclass(frozen=True)
class Profile:
    m: int = 16
    rate: str = '3/4'
    n: int = 8
    peak: int = 128                 # worst-case complex envelope, DAC counts
    frac: int = 0                   # 0: packed 10-bit fields; 3: packed 16-bit fields
    alpha: float = .20

    def __post_init__(self):
        if self.n not in (4, 8) or self.frac not in (0, 3) or not 1 <= self.peak <= 500:
            raise ValueError('n=4/8, frac=0/3, peak=1..500')

    @cached_property
    def points(self):
        return constellation(self.m, self.rate)

    @cached_property
    def levels(self):
        return np.round(np.array([self.points.real, self.points.imag]).T * 2**LEVEL_FRAC).astype(np.int64)

    @cached_property
    def taps(self):
        g = s.window_rrc(self.alpha, self.n)
        # Triangle inequality: |sum x_a*g_a| <= max|x| * sum|g_a|.
        worst = np.abs(g.reshape(L, self.n)).sum(axis=0).max() * np.abs(self.points).max()
        return np.round(g * self.peak / worst * 2**TAP_FRAC).astype(np.int64)

    @property
    def field(self):
        return 10 if self.frac == 0 else 16

    @cached_property
    def single(self):
        c = self.taps.reshape(L, self.n)
        raw = rounded(np.einsum('wc,jp->jwpc', self.levels, c),
                      LEVEL_FRAC + TAP_FRAC - self.frac)
        return self._pack(raw, 16*self.n)

    def _pack(self, raw, stride):
        # raw[group,code,phase,IQ]. Biases include offset binary and final rounding.
        bias = -raw.min(axis=1)
        bias[0] += (512 << self.frac) + (4 if self.frac else 0) - bias.sum(axis=0)
        assert (raw.min(axis=1) + bias >= 0).all()
        assert ((raw.max(axis=1) + bias).sum(axis=0) < 1 << self.field).all()
        r = raw + bias[:, None]
        words = (r[..., 0] | r[..., 1] << self.field).astype(np.uint32)
        table = np.zeros((raw.shape[1], stride), np.uint32)
        table[:, :raw.shape[0]*self.n] = words.transpose(1, 0, 2).reshape(raw.shape[1], -1)
        return table

    @cached_property
    def pair(self):
        c = self.taps.reshape(L, self.n)
        # One common row alphabet contains M^2 ordered pairs; tail has its own alphabet.
        ids = np.arange(self.m*self.m)
        lev = self.levels
        raw = np.empty((4, len(ids), self.n, 2), np.int64)
        for j in range(4):
            v = (lev[ids % self.m, None]*c[2*j, :, None]
                 + lev[ids // self.m, None]*c[2*j+1, :, None])
            raw[j] = rounded(v, LEVEL_FRAC+TAP_FRAC-self.frac)
        tail = rounded(lev[:, None]*c[8, :, None], LEVEL_FRAC+TAP_FRAC-self.frac)
        # Tail extrema include QPSK so that both tables share the same envelope guarantee.
        bias = -raw[:4].min(axis=1)
        bt = -tail.min(axis=0)
        bias[0] += (512 << self.frac) + (4 if self.frac else 0) - bias.sum(axis=0) - bt
        assert (raw[:4].min(axis=1) + bias >= 0).all()
        assert ((raw[:4].max(axis=1)+bias).sum(axis=0) + tail.max(axis=0)+bt < 1 << self.field).all()
        v = raw[:4] + bias[:, None]
        tab = (v[..., 0] | v[..., 1] << self.field).astype(np.uint32)
        tab = tab.transpose(1, 0, 2).reshape(self.m*self.m, 4*self.n)
        v = tail + bt
        return tab, np.ascontiguousarray(v[..., 0] | v[..., 1] << self.field, dtype=np.uint32)

    def pack_sum(self, w):
        w = np.asarray(w, np.uint32)
        if self.frac:
            w = ((w >> self.frac) & 1023) | ((w >> (self.field+self.frac-10)) & 0xffc00)
        return w ^ s.XOR


def shape(p, sym, method='single'):
    if method not in ('single', 'pair'):
        raise ValueError('method must be single or pair')
    sym = np.asarray(sym, np.int64)
    if len(sym) < HISTORY or np.any(sym < 0) or np.any(sym >= p.m+4):
        raise ValueError('eight history symbols and valid codes required')
    out = np.zeros((len(sym)-HISTORY, p.n), np.uint32)
    for age in range(9 if method == 'single' else 5):
        if method == 'single':
            ids = sym[HISTORY-age:len(sym)-age]
            out += p.single[ids, age*p.n:(age+1)*p.n]
        else:
            if (sym >= p.m).any():
                raise ValueError('pair prototype is data only; use single at PL transitions')
            if age < 4:
                a = 2*age
                ids = sym[HISTORY-a:len(sym)-a] + p.m*sym[HISTORY-a-1:len(sym)-a-1]
                out += p.pair[0][ids, age*p.n:(age+1)*p.n]
            else:
                out += p.pair[1][sym[:len(sym)-HISTORY]]
    return p.pack_sum(out).ravel()


def linear(p, sym):
    v = p.levels[np.asarray(sym)]
    x = (v[:, 0] + 1j*v[:, 1]) / 2**LEVEL_FRAC
    u = np.zeros(len(sym)*p.n, complex)
    u[::p.n] = x
    return np.convolve(u, p.taps / 2**TAP_FRAC)[HISTORY*p.n:len(sym)*p.n]


def cyclic(p, count=32768, seed=2026):
    x = np.random.default_rng(seed).integers(0, p.m, count, dtype=np.uint8)
    return np.r_[x[-8:], x]


def spectrum(p, method, nfft=32768):
    """Exact IID ensemble PSD of the row sum, before the final Q3 rounding.

    Single rows are functions of one independent symbol. Pair rows decompose
    into two conditional means and an interaction with zero conditional mean.
    Overlapping pairs share at most one symbol, so their interactions have zero
    covariance. Interactions for the same pair add with their two-symbol delays.
    FFTs include both I and Q, and are averaged across all symbols/pairs.
    """
    def values(t):
        return ((t & ((1 << p.field)-1)).astype(float) + 1j*(t >> p.field))/2**p.frac
    def power(rows):
        result=np.zeros(nfft)
        for block in np.array_split(rows, max(1,(len(rows)+15)//16)):
            result += (abs(np.fft.fft(block,nfft,axis=1))**2).sum(axis=0)
        return result/(len(rows)*p.n)
    if method=='single':
        v=values(p.single[:p.m,:9*p.n])
        return power(v-v.mean(axis=0))
    tab,tail=p.pair
    v=values(tab).reshape(p.m,p.m,4,p.n)  # older, newer, group, phase
    mean=v.mean(axis=(0,1))
    newer=v.mean(axis=0)-mean
    older=v.mean(axis=1)-mean
    one=np.zeros((p.m,9*p.n),complex)
    interaction=np.zeros((p.m,p.m,9*p.n),complex)
    for j in range(4):
        one[:,2*j*p.n:(2*j+1)*p.n]=newer[:,j]
        one[:,(2*j+1)*p.n:(2*j+2)*p.n]=older[:,j]
        interaction[:,:,2*j*p.n:(2*j+1)*p.n]=(v[:,:,j]-mean[j]
                -newer[None,:,j]-older[:,None,j])
    t=values(tail[:p.m]);one[:,8*p.n:]=t-t.mean(axis=0)
    return power(one)+power(interaction.reshape(p.m*p.m,-1))


def total_mask(p, method, sym, y, nfft=32768):
    """Continuous IID PSD mask, including table and final DAC rounding.

    Whole-count tables: exact ensemble PSD. Q3: fit the linear pulse with a
    nine-symbol least-squares projection, then add the Welch PSD of the residual.
    The residual is orthogonal to the symbols, so its cross spectrum with the
    fitted pulse vanishes in the IID ensemble limit. This avoids estimating a
    small cross term beside a much larger random linear-signal periodogram.
    Remove per-phase means before Welch; discrete DC/clock lines are separate.
    Q3 is a finite-record estimate, not a compliance measurement or hard bound.
    """
    from scipy.signal import welch
    if not p.frac:
        power=spectrum(p,method,nfft)
        base=power[0]
    else:
        x=p.points[sym]
        design=np.stack([x[8-age:len(sym)-age] for age in range(9)],axis=1)
        waveform=y.reshape(-1,p.n)
        gram=np.einsum('sa,sb->ab',design.conj(),design)
        rhs=np.einsum('sa,sp->ap',design.conj(),waveform)
        pulse=np.linalg.solve(gram,rhs)
        residual=waveform-np.einsum('sa,ap->sp',design,pulse)
        residual-=residual.mean(axis=0)
        _,noise=welch(residual.ravel(),nperseg=min(nfft,len(y)),nfft=nfft,
                      return_onesided=False,scaling='density',detrend=False)
        power=abs(np.fft.fft(pulse.ravel(),nfft))**2/p.n+noise
        base=abs(pulse.sum())**2/p.n
    f=np.fft.fftfreq(nfft)*2*p.n
    use=f>=0
    fn=f[use]
    db=10*np.log10(np.maximum(power[use]/base,1e-150)*np.sinc(fn/(2*p.n))**2)
    up,lo=[np.array(s.MASK[p.alpha][k]) for k in ['upper','lower']]
    upper=np.interp(fn,up[:,0],up[:,1],right=-40)-db
    sel=fn<=lo[-1,0]
    lower=db[sel]-np.interp(fn[sel],lo[:,0],lo[:,1])
    return float(min(upper.min(),lower.min()))


def metrics(p, method, count=32768):
    sym = cyclic(p, count)
    y = s.iq(shape(p, sym, method))
    exact = linear(p, sym)
    ref = p.points[sym[8:]]
    z = s.receive(p, y)
    gain = np.vdot(ref, z)/np.vdot(ref, ref)
    mer = 10*np.log10(np.mean(abs(ref)**2)/np.mean(abs(z/gain-ref)**2))
    # Ensemble response (conditional mean of a table row) and residual distortion PSD.
    # This separates deterministic filter ripple from finite-record random PSD variance.
    # Fit y = sum x*g_eff, with each age/phase independent for IID input symbols.
    x = p.points[:p.m]
    if method == 'single':
        tab = p.single[:p.m, :9*p.n].astype(np.int64)
        f = p.field
        vi = (tab & ((1 << f)-1)).astype(float)
        vq = (tab >> f).astype(float)
        v = vi + 1j*vq
        h = (x.conj()[:, None]*v).sum(axis=0)/(np.vdot(x,x)*2**p.frac)
    else:
        tab, tail = p.pair
        v = (tab & ((1 << p.field)-1)).astype(float) + 1j*(tab >> p.field)
        h = np.zeros(9*p.n, complex)
        ids = np.arange(p.m*p.m)
        for j in range(4):
            for a, xx in enumerate([x[ids%p.m], x[ids//p.m]]):
                h[(2*j+a)*p.n:(2*j+a+1)*p.n] = (xx.conj()[:, None]*v[:, j*p.n:(j+1)*p.n]).sum(axis=0) / (np.vdot(xx,xx)*2**p.frac)
        v = (tail[:p.m] & ((1 << p.field)-1)).astype(float) + 1j*(tail[:p.m] >> p.field)
        h[8*p.n:] = (x.conj()[:, None]*v).sum(axis=0) / (np.vdot(x,x)*2**p.frac)
    # response_db assumes symmetric real taps; symmetry follows the circular constellations.
    assert np.max(abs(h.imag)) < 1e-8
    margin = s.mask_margins(h.real, p.n, p.alpha)
    # Error PSD relative to exact waveform. A diagnostic, not a compliance measurement.
    f = np.abs(np.fft.fftfreq(len(y))*2*p.n)
    bands = (f*16).astype(int)
    err = y-exact
    noise = np.bincount(bands, abs(np.fft.fft(err))**2)/np.bincount(bands)
    inband = np.mean(abs(np.fft.fft(y)[f<.4])**2)
    far = np.unique(bands[f>=s.MASK[p.alpha]['upper'][-1][0]])
    return dict(basis='host model', symbol_count=count, seed=2026, alpha=p.alpha,
                m=p.m, rate=p.rate, n=p.n, peak=p.peak, frac=p.frac, method=method,
                table_bytes=p.single.nbytes if method=='single' else sum(t.nbytes for t in p.pair),
                mer_db=float(mer), mask_total_estimate_db=total_mask(p,method,sym,y), mask_linear_db=float(min(margin[k] for k in ['upper','lower','far'])),
                rounding_floor_db=float(10*np.log10(noise[far].max()/inband)),
                papr_db=float(10*np.log10(max(abs(y)**2)/np.mean(abs(y)**2))),
                rms_complex_lsb=float(np.sqrt(np.mean(abs(y)**2))),
                observed_peak_lsb=float(max(abs(y))),
                dc_dbc=float(10*np.log10(abs(y.mean())**2/np.mean(abs(y)**2)+1e-300)),
                max_component_error_lsb=float(max(abs(err.real).max(),abs(err.imag).max())))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--count',type=int,default=32768)
    ap.add_argument('--out')
    ap.add_argument('--all-rates',action='store_true')
    ap.add_argument('--peak',type=int,default=128)
    ap.add_argument('--alpha',type=float,choices=[.20,.35],default=.20)
    args=ap.parse_args()
    lines=[]
    for m in [16,32]:
        rates=(GAMMA16 if m==16 else GAMMA32) if args.all_rates else ['3/4']
        for rate in rates:
            for n in [4,8]:
                for frac in [0,3]:
                    p=Profile(m=m,rate=rate,n=n,frac=frac,peak=args.peak,alpha=args.alpha)
                    for method in ['single','pair']:
                        r=metrics(p,method,args.count)
                        line=json.dumps(r,sort_keys=True); print(line,flush=True); lines.append(line)
    if args.out:
        from pathlib import Path
        Path(args.out).write_text('\n'.join(lines)+'\n')

if __name__=='__main__':
    main()
