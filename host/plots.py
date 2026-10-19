#!/usr/bin/env python3
"""Regenerate the five documentation figures from recorded runs; no board access."""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

from plotstyle import BLUE, DARK, GREEN, INK, MID, RED, WIDTH, lollipop, plt, save_figure

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'docs/measurements'


def read_source(source):
    if source['ref'] is None:
        return (ROOT / source['path']).read_bytes()
    result = subprocess.run(['git', 'show', f"{source['ref']}:{source['path']}"],
                            cwd=ROOT, capture_output=True, check=False)
    if result.returncode:
        raise ValueError(f"Missing archive source {source['path']}; fetch tag archive/dev-2026-10-17")
    return result.stdout


def load_points():
    """Verify every snapshot against its original file and exact 1-based line(s)."""
    table = json.loads((DATA / 'plot-data.json').read_text())
    for source in json.loads((DATA / 'sources.json').read_text())['records']:
        raw = (DATA / source['file']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == source['sha256'], source['file']
        if 'records' in source:
            assert len(raw.splitlines()) == source['records'], source['file']
    for context in [table['cpu_source'], *[d['source'] for d in table['deadlines'].values()]]:
        assert context['quote'] in read_source(context).decode(), context['path']
    assert f"{table['cpu_hz'] / 1e6:g} MHz" in table['cpu_source']['quote']
    for deadline in table['deadlines'].values():
        assert f"{deadline['cycles']:,}" in deadline['source']['quote']
    sources = {}
    for key, source in table['sources'].items():
        raw = read_source(source)
        assert hashlib.sha256(raw).hexdigest() == source['sha256'], source['path']
        sources[key] = [json.loads(line) for line in raw.splitlines()]
    for key, point in table['points'].items():
        records = [sources[point['source']][i-1] for i in point['lines']]
        for field, expected in point['values'].items():
            actual = []
            for record in records:
                value = record
                for part in field.split('.'):
                    value = value[part]
                actual.append(value)
            assert actual == (expected if isinstance(expected, list) else [expected]*len(records)), (key, field)
        # Explicitly exclude the still-unqualified 8PSK 26.67 Msym/s path.
        row = point['values']
        assert row['modcod'] < 12 or row.get('symbol_rate', 0) <= 20_000_000, key
    return table


def symbol_rate(row):
    """Use the recorded sample ratio for fractional rates, not rounded telemetry."""
    if 'sps_num' in row:
        return Fraction(row['dac_rate'] * row.get('sps_den', 1), row['sps_num'])
    if 'dac_rate' in row and 'sps' in row:
        return Fraction(row['dac_rate']) / Fraction(str(row['sps']))
    return Fraction(row['symbol_rate'])


def payload(point):
    row = point['values']
    return float(8 * row['payload_bytes_per_frame'] * symbol_rate(row) / row['frame_symbols'] / 1_000_000)


def appearance(point):
    return (BLUE, 's') if point['values']['modcod'] < 12 else (GREEN, 'o')


def mode_label(point):
    return f"{point['mode']}, {point['rate_label']} Msym/s"


def selected_figures(table, out):
    rows = [table['points'][key] for key in table['figures']['fill']]
    labels = [mode_label(p) + (' (older pulse)' if p['values']['modcod'] >= 12 else '') for p in rows]
    cpu_us = table['cpu_hz'] / 1e6
    for name in ('fill', 'payload'):
        fig, ax = plt.subplots(figsize=(WIDTH, 3.0))
        if name == 'fill':
            values = [p['values']['max_fill_cycles']/cpu_us for p in rows]
            deadline = table['deadlines']['fast']['cycles']/cpu_us
            ax.axvline(deadline, color=RED, linestyle='--', linewidth=0.7)
            ax.set_xlabel('Longest measured bank fill (µs at 240 MHz)')
            ax.set_xlim(0, 225)
            ax.text(deadline-2, -.6, f'{deadline:.2f} µs deadline', ha='right', fontsize=8, color=RED)
            title = 'Bank-fill time, selected keyed records'
        else:
            values = [payload(p) for p in rows]
            ax.set_xlabel('Calculated payload rate during data (Mb/s)')
            ax.set_xlim(0, 67)
            title = 'Payload rate, selected keyed records'
        for y, (value, point) in enumerate(zip(values, rows)):
            color, marker = appearance(point)
            lollipop(ax, y, value, color, marker=marker,
                     offset=-5 if name == 'fill' and value > 180 else 5)
        ax.set(yticks=range(len(rows)), yticklabels=labels, ylim=(len(rows)-.4, -1))
        ax.grid(axis='x')
        ax.set_title(title)
        save_figure(fig, out/(name+'.png'),
                    'Shielded box, 30 s including ID; historical firmware. 8PSK uses the earlier pulse.\n'
                    'Payload rates exclude Morse ID; timing evidence does not establish RF decoding.')


def rate_history(table, out):
    rows = [table['points'][key] for key in table['figures']['rate-history']]
    fig, ax = plt.subplots(figsize=(WIDTH, 4.9))
    for y, point in enumerate(rows):
        color, marker = appearance(point)
        lollipop(ax, y, payload(point), color, marker=marker, fmt='{:.2f}')
    ax.set(yticks=range(len(rows)),
           yticklabels=[f"{p['label']} · {mode_label(p)}" for p in rows],
           ylim=(len(rows)-.4, -.6), xlim=(0, 69),
           xlabel='Calculated payload rate during data (Mb/s)',
           title='From the first stream to 58.2 Mb/s')
    ax.grid(axis='x')
    save_figure(fig, out/'rate-history.png',
                'Development order, 16–17 Oct 2026; one recorded run per point. First row: radio off; others: keyed.\n'
                'Normal frames with pilots; rates exclude Morse ID. Historical pulses and firmware differ.')


def encoder_history(table, out):
    groups = table['figures']['encoder-history']
    rows = [table['points'][key] for key in groups['8psk']]
    values = [statistics.median(p['values']['cycles.total'])/1000 if 'cycles.total' in p['values']
              else p['values']['encode_cycles_mean']/1000 for p in rows]
    fig, (ax, bx) = plt.subplots(1, 2, figsize=(WIDTH, 3.25),
                                gridspec_kw={'width_ratios': [1.35, 1]})
    for x, value in enumerate(values):
        ax.plot(x, value, marker='D' if x == 0 else 'o', color=INK if x == 0 else BLUE,
                linestyle='none')
        ax.annotate(f'{value:,.0f}k', (x, value), xytext=(0, 7), textcoords='offset points',
                    ha='center', fontsize=8)
    ax.set_yscale('log')
    ax.set(xticks=range(4), xticklabels=['C loops', 'PSRAM\nring', 'Packed\nring', 'RAM\nworkspace'],
           xlim=(-.4, 3.4), ylim=(100, 4000), ylabel='Cycles per frame (×10³, log scale)',
           title='(a) 8PSK 2/3')
    ax.set_yticks([100, 300, 1000, 3000], ['100', '300', '1000', '3000'])
    ax.minorticks_off()
    ax.grid(axis='y')
    rows = [table['points'][key] for key in groups['qpsk']]
    for x, point in enumerate(rows):
        value = point['values']['encode_cycles_mean']/1000
        bx.plot(x, value, marker='s', color=BLUE, linestyle='none')
        bx.annotate(f'{value:.0f}k', (x, value), xytext=(0, -14), textcoords='offset points',
                    ha='center', fontsize=8)
    row = rows[-1]['values']
    budget = float(table['cpu_hz'] * row['frame_symbols'] / symbol_rate(row) / 1000)
    bx.axhline(budget, color=MID, linestyle='--', linewidth=.7)
    bx.text(.5, budget+12, f'{budget:.1f}k frame budget', ha='center', color=DARK, fontsize=8)
    bx.set(xticks=[0, 1], xticklabels=['First 40 MHz', '15% reserve'], xlim=(-.4, 1.4),
           ylim=(0, 300), ylabel='Mean cycles per frame (×10³)', title='(b) QPSK 9/10, keyed')
    bx.grid(axis='y')
    save_figure(fig, out/'encoder-history.png',
                '240 MHz. (a) C: isolated five-frame median; others: radio-off stream means (5, 10, 10 Msym/s).\n'
                '(b) Keyed means at 100/3 Msym/s. Different workloads; markers show recorded points only.')


def fill_history(table, out):
    rows = [table['points'][key] for key in table['figures']['fill-history']]
    fig, ax = plt.subplots(figsize=(WIDTH, 5.1))
    cpu_us = table['cpu_hz']/1e6
    for y, point in enumerate(rows):
        fill = point['values']['max_fill_cycles']/cpu_us
        deadline = table['deadlines'][point['deadline']]['cycles']/cpu_us
        color, marker = appearance(point)
        if point['values']['status'] != 'ESP_OK':
            color, marker = RED, 'x'
        ax.hlines(y, 0, fill, color=color, linewidth=.7)
        ax.plot(fill, y, marker=marker, color=color, linestyle='none',
                label='Longest fill' if y == 0 else None)
        ax.plot(deadline, y, marker='|', color=RED, markersize=9, linestyle='none',
                label='Bank deadline' if y == 0 else None)
        ax.annotate(f'{fill:.1f} / {deadline:.2f}', (max(fill, deadline), y),
                    xytext=(6, 0), textcoords='offset points', va='center', fontsize=8)
    labels = [f"{p.get('fill_label', p['label'])} · {mode_label(p)}" for p in rows]
    ax.set(yticks=range(len(rows)), yticklabels=labels, ylim=(len(rows)-.4, -1.1),
           xlim=(0, 630), xticks=[0, 100, 200, 300, 400, 500, 600],
           xlabel='Bank-fill time / deadline (µs at 240 MHz)',
           title='Keeping each bank ahead of its deadline')
    ax.grid(axis='x')
    ax.legend(loc='upper right', ncol=2, handlelength=1)
    save_figure(fig, out/'fill-history.png',
                'Development milestones, 16–17 Oct 2026. Labels: longest fill / bank period; × marks the failed attempt.\n'
                'First stream, failed attempt and overlap/cache row: radio off; all others: keyed. Firmware differs.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT/'docs/img')
    parser.add_argument('--check', action='store_true', help='validate all point provenance without drawing')
    args = parser.parse_args()
    table = load_points()
    if args.check:
        print(f"Verified {len(table['points'])} points against exact source lines and file hashes.")
        return
    selected_figures(table, args.out)
    rate_history(table, args.out)
    encoder_history(table, args.out)
    fill_history(table, args.out)


if __name__ == '__main__':
    main()
