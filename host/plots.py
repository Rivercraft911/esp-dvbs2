#!/usr/bin/env python3
"""Regenerate docs/img/{fill,payload}.png from the small preserved evidence set."""
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT/'docs/measurements'
IMG = ROOT/'docs/img'


def main():
    rows = []
    for source in json.loads((DATA/'sources.json').read_text())['records']:
        raw = (DATA/source['file']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == source['sha256'], source['file']
        records = [json.loads(line) for line in raw.splitlines()]
        assert len(records) == source['records']
        rows.extend(r for r in records if r.get('cmd') == 'dvb')
    rows.sort(key=lambda r: (r['modcod'] >= 12, -r['symbol_rate'], r['modcod']))
    labels = []
    for r in rows:
        modulation = 'QPSK 9/10' if r['modcod'] == 11 else '8PSK 2/3' if r['modcod'] == 13 else '8PSK 5/6'
        labels.append(f"{modulation}, {r['symbol_rate']/1e6:.2f} Msym/s" +
                      (' (older pulse)' if r['modcod'] >= 12 else ''))
    plt.rcParams.update({'font.size': 9, 'axes.spines.top': False, 'axes.spines.right': False})
    IMG.mkdir(exist_ok=True)
    color = ['#32678e' if r['modcod'] < 12 else '#738b63' for r in rows]
    for name in ('fill', 'payload'):
        fig, ax = plt.subplots(figsize=(9.0, 3.0), layout='constrained')
        if name == 'fill':
            values = [r['max_fill_cycles']/240 for r in rows]
            ax.barh(labels, values, color=color)
            ax.axvline(15740/80, color='#ad4035', linestyle='--', label='196.75 µs deadline')
            ax.set_xlabel('Longest measured bank fill (µs at 240 MHz)')
            ax.set_xlim(0, 225)
            ax.text(15740/80+1.5, .6, 'deadline', rotation=90, va='center', fontsize=8, color='#ad4035')
        else:
            # Fractional symbol rate is derived from the exact sample ratio, not rounded telemetry.
            rates = [r['dac_rate']*r.get('sps_den', 1)/r.get('sps_num', r.get('sps', r['dac_rate']/r['symbol_rate']))
                     for r in rows]
            values = [8*r['payload_bytes_per_frame']*rs/r['frame_symbols']/1e6 for r,rs in zip(rows,rates)]
            ax.barh(labels, values, color=color)
            ax.set_xlabel('Calculated payload rate during data, excluding Morse ID (Mb/s)')
            ax.set_xlim(0, 67)
        for y, value in enumerate(values):
            ax.text(value+1, y, f'{value:.1f}', va='center', fontsize=8)
        ax.invert_yaxis()
        ax.set_title('Selected shielded-box keyed records, 30 s including ID', loc='left', fontsize=10)
        fig.savefig(IMG/(name+'.png'), dpi=160)
        plt.close(fig)
        print(IMG/(name+'.png'))


if __name__ == '__main__':
    main()
