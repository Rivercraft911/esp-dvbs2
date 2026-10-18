#!/usr/bin/env python3
"""Regenerate docs/img/{fill,payload}.png from the small preserved evidence set."""
import hashlib
import json
from pathlib import Path

from plotstyle import BLUE, GREEN, RED, WIDTH, lollipop, plt, save_figure

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
    # sources.json lists the newest record first; keep one record per mode and rate.
    seen = set()
    rows = [r for r in rows if not ((r['modcod'], r['symbol_rate']) in seen or seen.add((r['modcod'], r['symbol_rate'])))]
    rows.sort(key=lambda r: (r['modcod'] >= 12, -r['symbol_rate'], r['modcod']))
    labels = []
    for r in rows:
        modulation = 'QPSK 9/10' if r['modcod'] == 11 else '8PSK 2/3' if r['modcod'] == 13 else '8PSK 5/6'
        labels.append(f"{modulation}, {r['symbol_rate']/1e6:.2f} Msym/s" +
                      (' (older pulse)' if r['modcod'] >= 12 else ''))
    IMG.mkdir(exist_ok=True)
    color = [BLUE if r['modcod'] < 12 else GREEN for r in rows]
    for name in ('fill', 'payload'):
        fig, ax = plt.subplots(figsize=(WIDTH, 3.0))
        if name == 'fill':
            values = [r['max_fill_cycles']/240 for r in rows]
            ax.axvline(15740/80, color=RED, linestyle='--', linewidth=0.7)
            ax.set_xlabel('Longest measured bank fill (µs at 240 MHz)')
            ax.set_xlim(0, 225)
            ax.text(15740/80-2, -.6, '196.75 µs deadline', ha='right', fontsize=8, color=RED)
            title = 'Bank-fill time, selected keyed records'
        else:
            # Fractional symbol rate is derived from the exact sample ratio, not rounded telemetry.
            rates = [r['dac_rate']*r.get('sps_den', 1)/r.get('sps_num', r.get('sps', r['dac_rate']/r['symbol_rate']))
                     for r in rows]
            values = [8*r['payload_bytes_per_frame']*rs/r['frame_symbols']/1e6 for r,rs in zip(rows,rates)]
            ax.set_xlabel('Calculated payload rate during data (Mb/s)')
            ax.set_xlim(0, 67)
            title = 'Payload rate, selected keyed records'
        for y, (value, col, r) in enumerate(zip(values, color, rows)):
            lollipop(ax, y, value, col, marker='s' if r['modcod'] < 12 else 'o',
                     offset=-5 if name == 'fill' and value > 180 else 5)
        ax.set(yticks=range(len(rows)), yticklabels=labels, ylim=(len(rows)-.4, -1))
        ax.grid(axis='x')
        ax.set_title(title)
        save_figure(fig, IMG/(name+'.png'),
                    'Shielded box, 30 s including ID; historical firmware. 8PSK uses the earlier pulse.\n'
                    'Payload rates exclude Morse ID; timing evidence does not establish RF decoding.')


if __name__ == '__main__':
    main()
