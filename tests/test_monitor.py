"""host/monitor.py: the receiver's monitor datagrams, the spectrum and the PigeonVision radio report."""
import base64
import json
import math
import pathlib
import queue
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "host"))
import monitor  # noqa: E402


def tone(bin_offset: float, amplitude: float = 0.5) -> bytes:
    """A complex tone `bin_offset` FFT bins above the centre, as int8 I/Q."""
    n = np.arange(monitor.FFT)
    x = amplitude * np.exp(2j * np.pi * bin_offset * n / monitor.FFT)
    iq = np.empty(2 * monitor.FFT, np.int8)
    iq[0::2], iq[1::2] = np.round(x.real * 127), np.round(x.imag * 127)
    return iq.tobytes()


def events(q):
    out = []
    while True:
        try:
            out.append(q.get_nowait())
        except queue.Empty:
            return out


def main():
    # A tone 256 FFT bins above the centre lands 64 of the 512 output bins above it. Its power,
    # summed over the bins and divided by the window's noise bandwidth, is the tone's own:
    # 20 log10(0.5 x 127/128) = -6.09 dBFS, where a full-scale complex tone reads 0 dB.
    db = monitor.spectrum(tone(256))
    assert len(db) == monitor.BINS and int(np.argmax(db)) == monitor.BINS // 2 + 64, int(np.argmax(db))
    w = monitor.WINDOW
    enbw = len(w) * (w ** 2).sum() / w.sum() ** 2
    total = 10 * math.log10((10 ** (db / 10)).sum() * monitor.FFT / monitor.BINS / enbw)
    assert abs(total - 20 * math.log10(0.5 * 127 / 128)) < 0.05, total
    assert db.max() - np.median(db) > 60

    m = monitor.Monitor()
    q = m.subscribe()
    assert events(q)[0][0] == 'hello'

    # Samples arrive in four chunks; the spectrum appears once the last one does, in any order.
    iq = tone(-512)
    for c in (2, 0, 3):
        m.datagram(monitor.pack(monitor.SAMPLE, 7, 0, monitor.SAMPLES.pack(7, c, 4, 16_000_000) + iq[c * 1024:(c + 1) * 1024]))
    assert events(q) == []
    m.datagram(monitor.pack(monitor.SAMPLE, 7, 0, monitor.SAMPLES.pack(7, 1, 4, 16_000_000) + iq[1024:2048]))
    (kind, spec), (kind2, wave) = events(q)
    assert kind == 'spectrum' and spec['rate'] == 16_000_000 and int(np.argmax(spec['db'])) == monitor.BINS // 2 - 128
    assert kind2 == 'samples' and base64.b64decode(wave['iq']) == iq[:2 * monitor.WAVE]   # the waveform's first 32 us

    # A snapshot that lost a chunk is forgotten once newer snapshots arrive.
    m.datagram(monitor.pack(monitor.SAMPLE, 8, 0, monitor.SAMPLES.pack(8, 0, 4, 16_000_000) + iq[:1024]))
    for snap in range(9, 15):
        for c in range(4):
            m.datagram(monitor.pack(monitor.SAMPLE, snap, 0, monitor.SAMPLES.pack(snap, c, 4, 16_000_000) + iq[c * 1024:(c + 1) * 1024]))
    assert 8 not in m.chunks and len(events(q)) == 12

    # Symbols pass through unchanged.
    sym = np.arange(-64, 64, dtype=np.int8).tobytes()
    m.datagram(monitor.pack(monitor.SYMBOL, 1, 0, monitor.SYMBOLS.pack(64, 3, 0) + sym))
    (kind, s), = events(q)
    assert kind == 'symbols' and s['bits'] == 3 and base64.b64decode(s['iq']) == sym

    # Stats: the payload rate comes from the packet count over the time between reports.
    base = dict(state='track', freq_mhz=2420.0, frames=1000, failed=0, mer_med_db=29.8)
    m.datagram(monitor.pack(monitor.STATS, 1, 0, json.dumps(base | dict(packets=10_000)).encode()), now=100.0)
    m.datagram(monitor.pack(monitor.STATS, 2, 0, json.dumps(base | dict(packets=10_000 + 1197)).encode()), now=100.2)
    stats = [e for k, e in events(q) if k == 'rx']
    assert 'mbps' not in stats[0] and abs(stats[1]['mbps'] - 1197 * 188 * 8 / 0.2 / 1e6) < 1e-9

    # The transmitter's status records arrive as type 4; its input rate comes from the byte count.
    m.datagram(monitor.pack(monitor.TX, 1, 0, json.dumps(dict(cmd='status', input_bytes=0)).encode()), now=200.0)
    m.datagram(monitor.pack(monitor.TX, 2, 0, json.dumps(dict(cmd='status', input_bytes=1_125_000)).encode()), now=201.0)
    tx = [e for k, e in events(q) if k == 'tx']
    assert len(tx) == 2 and 'input_mbps' not in tx[0] and abs(tx[1]['input_mbps'] - 9.0) < 1e-9

    # pv-capture's status file and pv view's health report become rates per second.
    cm5 = dict(lifecycle='running', outputs=dict(transport=dict(wire_bytes=0, spi=dict(messages=0))))
    m.on_cm5(cm5, 300.0)
    cm5['outputs']['transport'].update(wire_bytes=1_138_750, spi=dict(messages=855))
    m.on_cm5(cm5, 301.0)
    m.on_viewer(dict(metrics=dict(access_units=dict(A=0, B=0), corrupt_packets=0, transport_discontinuities=dict(A=0, B=1))), 300.0)
    m.on_viewer(dict(metrics=dict(access_units=dict(A=30, B=29), corrupt_packets=0, transport_discontinuities=dict(A=0, B=1))), 301.0)
    got = dict((k, e) for k, e in events(q))
    assert abs(got['cm5']['mbps'] - 9.11) < 1e-9 and got['cm5']['messages'] == 855
    assert got['viewer']['fps'] == dict(A=30.0, B=29.0) and got['viewer']['gaps'] == 1

    # Malformed datagrams are counted and ignored.
    bad = m.bad
    m.datagram(b'PVMN')
    m.datagram(monitor.pack(9, 1, 0, b''))
    m.datagram(monitor.HEADER.pack(b'XXXX', 1, 1, 0, 0) + b'{}')
    assert m.bad == bad + 3

    # PigeonVision's radio monitor requires fer == errors / frames exactly, and None at zero frames.
    m.received = None
    assert m.radio()['lock'] is False
    m.stats = dict(state='track', frames=999, failed=1, mer_med_db=29.8)
    m.received = time.monotonic()
    r = m.radio()
    assert r == dict(lock=True, snr=29.8, fec=dict(frames=1000, errors=1, fer=1 / 1000)), r
    m.stats = dict(state='acquire')
    assert m.radio()['fec'] == dict(frames=0, errors=0, fer=None) and m.radio()['lock'] is False

    # The demo receiver is made of the same datagrams and parses cleanly.
    d, m2 = monitor.Demo(), monitor.Monitor()
    for step in range(4):
        for datagram in d.datagrams(step):
            m2.datagram(datagram)
    assert m2.bad == 0 and m2.stats['modcod'] == 14 and m2.datagrams == 4 * 5 + 2 + 1   # one transmitter status
    print("monitor checks passed")


if __name__ == "__main__":
    main()
