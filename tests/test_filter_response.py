"""Synthetic RF spectra exercise the offline analyzer; no hardware or RF claims."""
import pathlib
import sys

import numpy as np

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'host'))
from filter_response import point, summarize

class Samples:
    def __init__(self,z): self.z=z
    def __len__(self): return len(self.z)
    def read(self,first,count): return self.z[first:first+count]

rate=61_440_000
n=4*32768
t=np.arange(n)/rate
rng=np.random.default_rng(2026)
noise=1e-5*(rng.normal(size=n)+1j*rng.normal(size=n))
points=[]
for offset in list(range(-20,0))+list(range(1,21)):
    f=offset*1e6
    carrier=51000
    amp=.1*10**(-.002*f*f/1e12/20)
    z=amp*(np.exp(2j*np.pi*(f+carrier)*t)+.01*np.exp(2j*np.pi*(-f+carrier)*t)
           +.003*np.exp(2j*np.pi*carrier*t))+noise
    p=point(Samples(z),rate)
    assert p['offset_mhz']==offset
    assert abs(p['power_dbfs']-20*np.log10(amp))<.01
    assert abs(p['image_dbc']+40)<.1 and abs(p['carrier_dbc']-20*np.log10(.003))<.1
    points.extend([p]*3)
s=summarize(points)
assert s['complete_36mhz'] and s['complete_40mhz']
assert abs(s['flatness_36mhz_db']-.002*(18**2-1))<.01
assert not summarize(points[:-1])['complete_40mhz']
for z in (np.ones(n,dtype=complex), 2*np.exp(2j*np.pi*1e6*t), noise,
          np.r_[np.exp(2j*np.pi*1.051e6*t[:n//2]),np.exp(2j*np.pi*2.051e6*t[n//2:])]):
    try: point(Samples(z),rate)
    except ValueError: pass
    else: raise AssertionError('bad capture accepted')
# Shifted receiver LO retains the RF offset and moves receiver DC away from TX LO.
z=.1*np.exp(2j*np.pi*(-18e6+51000-6e6)*t)+noise
assert point(Samples(z),rate,6e6)['offset_mhz']==-18
print('filter response: +/-20 MHz, absolute power, image/carrier, completeness, LO shift and bad bursts pass (synthetic only)')
