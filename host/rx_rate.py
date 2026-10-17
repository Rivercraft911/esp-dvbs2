#!/usr/bin/env python3
"""COORDINATOR ONLY: qualify sustained E200 delivery rate. Does not prove sample continuity."""
import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

import sdr


def stream_command(radio):
    return [radio.tool("iio_readdev"), "-u", radio.settings.uri, "-T", "5000", "-b", "262144",
            "cf-ad9361-lpc", "voltage0", "voltage1"]


def drain(source, sample_rate, seconds):
    start = last = time.monotonic()
    total = previous = 0
    windows = []
    ended = False
    while time.monotonic()-start < seconds:
        ready = select.select([source], [], [], min(.2, max(0,seconds-(time.monotonic()-start))))[0]
        if ready:
            raw = os.read(source.fileno(), 4*262144)
            if not raw:
                ended = True
                break
            total += len(raw)
        now = time.monotonic()
        if now-last >= 1:
            windows.append(dict(elapsed_s=now-start, samples_per_second=(total-previous)/4/(now-last)))
            last, previous = now, total
    elapsed = time.monotonic()-start
    rate = total/4/elapsed
    return dict(elapsed_s=elapsed, iq_bytes=total, samples_per_second=rate,
                requested_samples_per_second=sample_rate, delivery_ratio=rate/sample_rate,
                windows=windows, early_eof=ended, partial_sample=bool(total%4),
                delivery_pass=not ended and total%4==0 and rate >= .98*sample_rate
                              and all(w['samples_per_second'] >= .98*sample_rate for w in windows[1:]),
                continuity_proven=False)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--uri',default='ip:192.168.1.10')
    p.add_argument('--sample-rate',type=int,required=True)
    p.add_argument('--lo',type=int,default=2420000000)
    p.add_argument('--bandwidth',type=int,default=13500000)
    p.add_argument('--gain',type=float,default=0)
    p.add_argument('--libiio',type=Path)
    p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--stdin',action='store_true',help='test an already configured producer, no device access')
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.sample_rate<=0 or not math.isfinite(a.seconds) or a.seconds<=0:
        p.error('positive sample rate and duration required')
    if a.out.exists():
        p.error('output already exists')
    report=dict(measured_at=dt.datetime.now(dt.timezone.utc).isoformat(),continuity_proven=False)
    proc=None
    a.out.parent.mkdir(parents=True,exist_ok=True)
    with a.out.open('x') as output, tempfile.TemporaryFile() as errors:
        try:
            if a.stdin:
                source=sys.stdin.buffer
            else:
                radio=sdr.SDR(sdr.Settings(uri=a.uri,lo_hz=a.lo,rate_hz=a.sample_rate,
                                          bandwidth_hz=a.bandwidth,gain_db=a.gain),a.libiio)
                report['configuration']=radio.configure()
                report['command']=stream_command(radio)
                proc=subprocess.Popen(report['command'],stdout=subprocess.PIPE,stderr=errors,env=radio.env,
                                      stdin=subprocess.DEVNULL,bufsize=0)
                source=proc.stdout
            report.update(drain(source,a.sample_rate,a.seconds))
            if proc and proc.poll() is not None:
                report['reader_exit']=proc.returncode
                report['delivery_pass']=False
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as exc:
            report.update(error=str(exc),delivery_pass=False)
        finally:
            if proc:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill();proc.wait()
                proc.stdout.close()
            errors.seek(0)
            report['stderr']=errors.read(65536).decode(errors='replace')
            json.dump(report,output,indent=2,allow_nan=False)
            output.write('\n')
    print(json.dumps(report,allow_nan=False))
    return 0 if report.get('delivery_pass') else 1


if __name__=='__main__':
    raise SystemExit(main())
