"""Independent SPI pattern, radio-off run gates and LeanDVB checks. Never opens a board port."""
import argparse
import json
import math
from pathlib import Path
import struct
import zlib

PAYLOAD = 1316


def pattern(seq, kind):
    seq &= 65535
    out = bytearray()
    for j in range(7):
        x = ((((seq << 8) | j) * 0x9e3779b1) & 0xffffffff) | 1
        p = bytearray()
        for _ in range(47):
            x ^= (x << 13) & 0xffffffff
            x ^= x >> 17
            x ^= (x << 5) & 0xffffffff
            p += struct.pack('<I', x)
        if kind == 1:
            p[:8] = bytes((0x47, 1, 0, 0x10 | ((seq*7+j) & 15), seq & 255, seq >> 8, j, 0xa5))
        out += p
    return bytes(out)


def record_check(r):
    """Raise on a failed gate. These ceilings are proposals, not measured input results."""
    command = r.get('command', '')
    if r.get('done') or 'cmd' not in r:
        return
    assert 'error' not in r, r
    if r['cmd'] == 'spirx':
        f = int(command.split()[5]) if len(command.split()) > 5 else 0
        s = r['spi']
        assert r['verified_msgs'] > 0 and r['bad_payload'] == 0, r
        assert r['verified_bytes'] == s['bytes'] == r['verified_msgs']*PAYLOAD, r
        assert s['back'] == s['wrong_type'] == 0, r
        assert r.get('sender_errors', 0) == 0, r
        names = ('short', 'long', 'header', 'crc', 'sync')
        if f in (0, 5, 6):
            assert all(s[k] == 0 for k in (*names, 'lost', 'overruns')), r
            want = 0
            for seq in range(r['verified_msgs']):
                want = zlib.crc32(pattern(seq, 1 if r['input'] == 'ts' else 2), want)
            assert want == r['crc32'], r
        if f in (1, 2, 3):
            name = {1: 'crc', 2: 'short', 3: 'long'}[f]
            assert s[name] > 0 and all(s[k] == 0 for k in names if k != name), r
            assert s['overruns'] == 0 and s['lost'] > 0, r
        if f == 4:
            assert s['overruns'] > 0 and s['restarts'] > 0, r
        if f == 5:
            assert s['ready_falls'] > 0 and r['sender_waits'] > 0, r
        if f == 0 and len(command.split()) > 4 and float(command.split()[4]):
            assert r['mbps'] >= .98*float(command.split()[4]), r
    if r['cmd'] in ('dvbdump', 'dvbpace', 'wifimem') and 'status' in r:
        assert r['status'] == 'ESP_OK' and not r['encoder_failed'], r
        if r.get('input') in ('ts', 'gcs'):
            s = r['spi']
            assert all(s[k] == 0 for k in ('short','long','header','crc','sync','lost','back','overruns','wrong_type')), r
            assert r['input_bytes'] > 0 and r['frames_encoded'] > 0, r
        if r['cmd'] == 'dvbdump':
            assert r['overlap_mismatches'] == 0 and r['underruns'] == 0, r
        if r['cmd'] == 'dvbpace':
            if command:
                assert r['seconds'] == int(command.split()[3]), r
            assert r['late_fills'] == r['late_switches'] == r['fatal_late'] == 0 and r['min_fill_slack_cycles'] > 0, r
            if r.get('input_fault') == 6 or 'starve' in r.get('test', ''):
                assert r['underruns'] > 0 and r['frames_encoded'] > 0, r
            else:
                assert r['underruns'] == 0, r
            if r.get('sps') == 3:
                # At 240 MHz: bank = 47,220 cycles; frame = 299,538 cycles.
                assert r['max_fill_cycles'] <= 40137 and r['min_fill_slack_cycles'] >= 7083, r
                assert r['encode_cycles_mean'] <= 254607 and r['encode_cycles_live_max'] <= 299538, r
        if r['cmd'] == 'wifimem':
            assert r['frames_played'] == r['positions'] == r['max_fill_call_cycles'] == 0, r
            assert r['wifi_free_up'] >= 10000 and r['wifi_min_free'] >= 10000, r


def decode_dump(path, kind, mc, sps, rate, frames, receiver):
    import tempfile
    import numpy as np
    import decode
    import dvbs2
    rate_code = dvbs2.RATES[mc-1] if mc < 12 else dvbs2.PSK8_RATES[mc-12]
    per_frame = (dvbs2.code(rate_code).kbch-80)//8
    expected = b''.join(pattern(seq, kind) for seq in range(math.ceil((frames+3)*per_frame/PAYLOAD)))
    data = np.fromfile(path, '<i2').reshape(-1, 2).astype(np.float32)/512
    with tempfile.TemporaryDirectory() as tmp:
        capture = Path(tmp)/'input.cf32'
        (data[:, 0]+1j*data[:, 1]).astype('<c8').tofile(capture)
        report, got = decode.decode(capture, sample_rate=rate, symbol_rate=rate/sps,
                                    receiver=receiver, rrc_rej=20)
    assert report['frame_lock'] and report['bb_frames'] >= frames-2, report
    for name in ('ldpc_bit_flips','ldpc_checks_before','ldpc_checks_after','bch_corrected_bits',
                 'bch_failed_frames','bbheader_rejected_frames'):
        assert report[name] == 0, report
    at = expected.find(got)
    assert got and at >= 0, 'decoded input differs from sender pattern'
    assert len(got) >= (frames-2)*per_frame - (376 if kind == 1 else 0), report
    assert at % (188 if kind == 1 else per_frame) == 0, at
    report.update(input_match=True, input_offset=at, input_bytes=len(got))
    print(json.dumps(report))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check')
    c.add_argument('records', type=Path)
    d = sub.add_parser('decode')
    d.add_argument('iq', type=Path)
    d.add_argument('--kind', choices=('ts','gcs'), required=True)
    d.add_argument('--modcod', type=int, default=11)
    d.add_argument('--sps', type=int, default=3)
    d.add_argument('--rate', type=int, default=80000000)
    d.add_argument('--frames', type=int, default=16)
    d.add_argument('--receiver', type=Path, required=True)
    a = p.parse_args()
    if a.cmd == 'check':
        rows = [json.loads(line) for line in a.records.read_text().splitlines()]
        checked = [r for r in rows if not r.get('done') and ('status' in r or r.get('cmd') == 'spirx')]
        assert checked, 'no results'
        for r in checked:
            record_check(r)
        print(f'{len(checked)} SPI/input results passed')
    else:
        decode_dump(a.iq, 1 if a.kind == 'ts' else 2, a.modcod, a.sps, a.rate, a.frames, a.receiver)


if __name__ == '__main__':
    main()
