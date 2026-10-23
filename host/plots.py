#!/usr/bin/env python3
"""Regenerate documentation figures from recorded runs; no board access."""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

import numpy as np

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


def rf_figures(out, *, iq_dir=None, check=False):
    """Plot compact power summaries of the original, single-capture B200 records."""
    folder = ROOT / 'results/2026-10-21-rf-figures'
    records = [json.loads(s) for s in (folder/'runs.jsonl').read_text().splitlines()]
    for row in records:
        source = [json.loads(s) for s in (ROOT/row['source_runs']).read_text().splitlines()]
        original = next(r for r in source if r.get('id') == row['source_record'])
        assert original['capture']['sha256'] == row['source_iq_sha256']
        assert source[0]['native_rate_hz'] == row['sample_rate_hz']
        assert source[0]['native_samples'] == row['samples']
        assert row['seconds'] == row['samples']/row['sample_rate_hz']
        assert (row['fft'], row['time_average'], row['frequency_average']) == (4096, 2, 4)
        assert original['modcod'] == row['modcod']
        assert original['firmware'] == row['firmware']
        assert original['source'] == 'prepared_repeated' and row['payload'] == 'prepared, repeated'
        assert original['nominal_bandwidth_hz'] == row['bandwidth_mhz']*1e6
        receiver = next(r for r in original['receivers'] if r['name'] == 'stock')
        assert receiver['strict_receiver_pass'] and receiver['decoded_frames'] == row['exact_frames']
        path = folder/row['file']
        if iq_dir is not None:
            raw = (iq_dir/row['id']/'capture.cf32').read_bytes()
            assert hashlib.sha256(raw).hexdigest() == row['source_iq_sha256']
            samples = np.frombuffer(raw, dtype='<c8').reshape(-1, row['fft'])
            window = np.hanning(row['fft'])
            power = np.abs(np.fft.fftshift(np.fft.fft(samples*window, axis=1), axes=1))**2
            power /= row['sample_rate_hz'] * np.sum(window**2)
            power = power.reshape(256, 2, 1024, 4).mean(axis=(1, 3))
            freq = np.fft.fftshift(np.fft.fftfreq(row['fft'], 1/row['sample_rate_hz']))
            freq = freq.reshape(1024, 4).mean(axis=1)
            np.savez_compressed(path, frequency_hz=freq.astype('<f4'),
                                psd=power.mean(axis=0).astype('<f4'),
                                waterfall=power.astype('<f4'))
        assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], path
        with np.load(path) as data:
            frequency = data['frequency_hz']/1e6 + row['center_mhz']
            psd = 10*np.log10(data['psd'])
            waterfall = 10*np.log10(data['waterfall'])
        assert waterfall.shape == (256, 1024) and np.isfinite(waterfall).all()
        if check:
            continue
        fig, (ax, bx) = plt.subplots(2, 1, figsize=(11, 6.2), sharex=True,
                                    gridspec_kw={'height_ratios': [1, 1.15], 'hspace': 0.22})
        mode = 'QPSK 9/10' if row['modcod'] == 11 else '8PSK 9/10'
        ax.plot(frequency, psd, color=BLUE, linewidth=0.9)
        ax.set(ylim=(-135, -80), ylabel='PSD (dBFS/Hz)',
               title=f"Transmitted by an ESP32-S3, {mode} at {row['symbol_rate_hz']/1e6:.2f} Msym/s "
                     f"({row['bandwidth_mhz']:g} MHz, {row['payload_mbps']:g} Mb/s)")
        ax.set_yticks([-130, -120, -110, -100, -90, -80])
        span = row['sample_rate_hz']/2e6
        bx.imshow(waterfall, origin='lower', aspect='auto', vmin=-130, vmax=-85, interpolation='nearest',
                  extent=(row['center_mhz']-span, row['center_mhz']+span, 0, row['seconds']*1000))
        bx.grid(False)
        bx.set(xlabel='Frequency (MHz)', ylabel='Time (ms)', title='Spectrogram, B200 at 50 MS/s',
               xlim=(row['center_mhz']-span, row['center_mhz']+span))
        bx.set_yticks([0, 10, 20, 30, 40])
        bx.ticklabel_format(axis='x', style='plain', useOffset=False)
        save_figure(fig, out/('rf-40mhz.png' if row['modcod'] == 11 else 'rf-36mhz.png'))


def rx_summary(row, capture):
    """Spectrum, spectrogram and constellation of one receiver capture (.sc8: int8 I, Q pairs,
    floor(s10 / 4), what the receiver's rx_unpack produces). The constellation comes from the
    receiver's own demodulator (firmware/components/dvbs2rx) built for the host."""
    import tempfile
    import rx_demod
    pairs = np.fromfile(capture, np.int8).reshape(-1, 2).astype(np.float32) + 0.5
    x = (pairs[:, 0] + 1j*pairs[:, 1]) / 128          # full scale: 128 int8 LSB
    n, rate = row['fft'], row['sample_rate_hz']
    ta, fa = row['time_average'], row['frequency_average']
    rows = len(x) // (n*ta) * ta
    window = np.hanning(n)
    power = np.abs(np.fft.fftshift(np.fft.fft(x[:rows*n].reshape(rows, n)*window, axis=1), axes=1))**2
    power /= rate * np.sum(window**2)
    psd = power.mean(axis=0).reshape(n//fa, fa).mean(axis=1)
    waterfall = power.reshape(rows//ta, ta, n//fa, fa).mean(axis=(1, 3))
    freq = np.fft.fftshift(np.fft.fftfreq(n, 1/rate)).reshape(n//fa, fa).mean(axis=1)
    keep = np.abs(freq) <= row['waterfall_span_mhz']*1e6
    with tempfile.TemporaryDirectory() as tmp:
        demod = rx_demod.Demod(rx_demod.build(tmp), row['modcod'], quick_lock=True,
                               rate=row['symbol_rate_hz'])
        demod.feed(rx_demod.sc8_words(capture))
    checks = rx_demod.check(demod.frames, row['modcod'], False, row['callsign'], limit=20000)
    assert len(checks) == row['frames'] and all(c['bit_errors'] == 0 for c in checks), capture
    points = []
    for frame in demod.frames:
        soft = frame['llr'].astype(float)
        if row['modcod'] >= 12:     # bits 2 and 3 are the point's axes after -pi/8 (dvbs2rx.h)
            planes = soft.reshape(3, -1)[::-1] if row['modcod'] == 12 else soft.reshape(3, -1)
            points.append((planes[1] + 1j*planes[2]) * np.sin(np.pi/8)/32 * np.exp(1j*np.pi/8))
        else:                       # 32 per nominal axis a / sqrt(2)
            points.append((soft[0::2] + 1j*soft[1::2]) / 32 / np.sqrt(2))
    z = np.concatenate(points)
    span = row['constellation_span']
    hist = np.histogram2d(z.real, z.imag, bins=row['constellation_bins'],
                          range=[[-span, span], [-span, span]])[0].T.astype('<u4')
    return dict(frequency_hz=freq.astype('<f4'), psd=psd.astype('<f4'),
                waterfall_frequency_hz=freq[keep].astype('<f4'),
                waterfall_db=(10*np.log10(waterfall[:, keep])).astype('<f2'), constellation=hist)


def rx_figures(out, *, iq_dir=None, check=False):
    """Plot what the receiving S3 saw: its own 16 MS/s capture and its demodulator's symbols."""
    from matplotlib.colors import LinearSegmentedColormap
    from scipy.ndimage import gaussian_filter
    from plotstyle import AXES
    folder = ROOT / 'results/2026-10-23-rx-figures'
    for row in [json.loads(s) for s in (folder/'runs.jsonl').read_text().splitlines()]:
        path = folder/row['file']
        if iq_dir is not None:
            capture = iq_dir/row['id']/'capture.sc8'
            assert hashlib.sha256(capture.read_bytes()).hexdigest() == row['capture_sha256']
            fresh = rx_summary(row, capture)
            if path.exists():
                with np.load(path) as data:
                    assert all(np.array_equal(data[k], v) for k, v in fresh.items()), path
            else:
                np.savez_compressed(path, **fresh)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], path
        with np.load(path) as data:
            frequency = data['frequency_hz']/1e6 + row['center_mhz']
            psd = 10*np.log10(data['psd'])
            waterfall = data['waterfall_db'].astype(float)
            columns = data['waterfall_frequency_hz']/1e6 + row['center_mhz']
            hist = data['constellation'].astype(float)
        assert np.isfinite(waterfall).all() and hist.sum() > 0
        if check:
            continue
        fig = plt.figure(figsize=(11, 5.1))
        grid = fig.add_gridspec(2, 2, width_ratios=[1.75, 1], height_ratios=[1, 1.15],
                                wspace=0.16, hspace=0.34)
        ax, bx, cx = fig.add_subplot(grid[0, 0]), fig.add_subplot(grid[1, 0]), fig.add_subplot(grid[:, 1])
        half = min(row['sample_rate_hz']/2e6, 1.6*row['bandwidth_mhz'])
        lim = (row['center_mhz']-half, row['center_mhz']+half)
        ax.plot(frequency, psd, color=BLUE, linewidth=0.9)
        inside = (frequency >= lim[0]) & (frequency <= lim[1])
        ax.set(xlim=lim, ylim=(np.floor(psd[inside].min()/5)*5 - 3, np.ceil(psd.max()/5)*5 + 3),
               ylabel='PSD (dBFS/Hz)',
               title=f"Received on an ESP32-S3, {row['mode']} at {row['symbol_rate_hz']/1e6:g} Msym/s")
        seconds = waterfall.shape[0] * row['fft'] * row['time_average'] / row['sample_rate_hz']
        step = columns[1] - columns[0]
        low, high = np.percentile(waterfall, [2, 99.7])
        bx.imshow(waterfall, origin='lower', aspect='auto', vmin=low, vmax=high, interpolation='nearest',
                  extent=(columns[0]-step/2, columns[-1]+step/2, 0, seconds*1000))
        bx.grid(False)
        bx.set(xlim=lim, xlabel='Frequency (MHz)', ylabel='Time (ms)', title='Spectrogram')
        for axis in (ax, bx):
            axis.ticklabel_format(axis='x', style='plain', useOffset=False)
        # Soft bits are integers, so points sit on a lattice (1/32 of the nominal axis amplitude);
        # smooth over about half a lattice step so the density shows the noise, not the lattice.
        span = row['constellation_span']
        lattice = np.sin(np.pi/8)/32 if row['modcod'] >= 12 else 1/32/np.sqrt(2)
        density = np.log10(gaussian_filter(hist, 0.6*lattice*row['constellation_bins']/(2*span)) + 0.05)
        cmap = LinearSegmentedColormap.from_list(
            'constellation', [AXES, '#0b2f57', '#1565c0', BLUE, '#a8dcff', '#ffffff'])
        cx.imshow(density, origin='lower', extent=(-span, span, -span, span), cmap=cmap,
                  interpolation='bicubic', vmin=np.log10(0.05))
        circle = np.linspace(0, 2*np.pi, 400)
        cx.plot(np.cos(circle), np.sin(circle), color='#ffffff', alpha=0.25, linewidth=0.6,
                linestyle='--', zorder=0)
        cx.set(xlim=(-span, span), ylim=(-span, span), aspect='equal', xlabel='In-phase',
               ylabel='Quadrature', title=f"Constellation, MER {row['data_mer_db_median']:.1f} dB")
        cx.set_xticks([-1, -0.5, 0, 0.5, 1])
        cx.set_yticks([-1, -0.5, 0, 0.5, 1])
        save_figure(fig, out/f"rx-{row['id'].split('-')[-1]}.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT/'docs/img')
    parser.add_argument('--check', action='store_true', help='validate all point provenance without drawing')
    parser.add_argument('--rf-only', action='store_true', help='only the capture figures (RF and receiver)')
    parser.add_argument('--rf-iq-dir', type=Path, help='rebuild RF summaries from original capture folders')
    parser.add_argument('--rx-iq-dir', type=Path, help='rebuild receiver summaries from original captures')
    args = parser.parse_args()
    rf_figures(args.out, iq_dir=args.rf_iq_dir, check=args.check)
    rx_figures(args.out, iq_dir=args.rx_iq_dir, check=args.check)
    if args.rf_only:
        return
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
