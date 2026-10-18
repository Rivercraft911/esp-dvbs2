"""Independent SPI pattern, radio-off run gates and LeanDVB checks. Never opens a board port."""
import argparse
import json
import math
from pathlib import Path
import struct
import zlib

PAYLOAD = 1316


def pv_pattern(seq):
    seq &= 65535
    return b''.join(bytes((0x47, 1, 0, 0x10 | ((7*seq+i) & 15))) +
                    bytes(((7*seq+i) % 112+j) & 255 for j in range(184)) for i in range(7))


def pv_chain(messages):
    chain = 0
    for seq in range(messages):
        body = struct.pack('<HBBHHI', 0x5650, 1, 1, seq & 65535, PAYLOAD, 0) + pv_pattern(seq)
        chain = zlib.crc32(struct.pack('<I', zlib.crc32(body)), chain)
    return chain


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
    """Check pattern integrity, transport counters and radio-off deadlines."""
    command = r.get('command', '')
    if r.get('done') or 'cmd' not in r:
        return
    assert 'error' not in r, r
    if r.get('input_loop') and not r.get('input_fault'):
        s = r['spi']
        assert s['messages'] == s['msgs'] > 0, r
        assert s['payload_bytes'] == s['bytes'] == PAYLOAD*s['messages'], r
        assert s['next_seq'] == (s['messages'] & 65535), r
        assert s['ts_packets'] == (7*s['messages'] if r['input'] == 'ts' else 0), r
        if r['input'] == 'gcs':
            assert s['crc_chain'] == 0, r
        elif r.get('input_wire') == 'raw':
            assert s['crc_chain'] == pv_chain(s['messages']), r
    if 'sender' in r:
        assert r['sender']['cpu_timed'] and r['sender']['errors'] == 0, r
        assert r['sender']['stack_min_free'] >= 256, r
        assert r['sender']['priority'] < 5, r
        assert r['sender']['core'] == (1 if r['cmd'] == 'spirx' else 0), r
    if r['cmd'] == 'spirx':
        f = r.get('input_fault', 0)
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
                payload = (pv_pattern(seq) if r.get('input_wire') == 'raw' and r['input'] == 'ts'
                           else pattern(seq, 1 if r['input'] == 'ts' else 2))
                want = zlib.crc32(payload, want)
            assert want == r['crc32'], r
            if r.get('input_wire') == 'raw' and r['input'] == 'ts' and not r.get('input_loop'):
                assert s['messages'] == r['verified_msgs'] and s['ts_packets'] == 7*s['messages'], r
                assert s['payload_bytes'] == s['bytes'] and s['next_seq'] == (s['messages'] & 65535), r
                assert s['crc_chain'] == pv_chain(s['messages']), r
        if f in (1, 2, 3):
            name = {1: 'crc', 2: 'short', 3: 'long'}[f]
            assert s[name] > 0 and all(s[k] == 0 for k in names if k != name), r
            assert s['lost'] > 0, r
            if f != 3:
                assert s['overruns'] == 0, r
            elif s['overruns']:
                assert s['restarts'] > 0, r
        if f == 4:
            assert s['overruns'] > 0 and s['restarts'] > 0, r
        if f == 5:
            assert s['ready_falls'] > 0 and r['sender_waits'] > 0, r
        if f == 0 and r.get('input_target', 0):
            assert r['mbps'] >= .98*r['input_target'], r
    if r['cmd'] in ('dvbdump', 'dvbpace') and 'status' in r:
        assert r['status'] == 'ESP_OK' and not r['encoder_failed'], r
        if 'encoder_stack_min_free' in r:
            assert r['encoder_stack_min_free'] >= 256, r
        if r.get('input') in ('ts', 'gcs'):
            s = r['spi']
            assert all(s[k] == 0 for k in ('short','long','header','crc','sync','lost','back','overruns','wrong_type')), r
            assert r['input_bytes'] > 0 and r['frames_encoded'] > 0, r
            assert r.get('input_borrowed_slots', 0) in (0, 4), r
            assert r.get('input_copy_cycles', 0) <= r.get('input_cycles', 0), r
        if r['cmd'] == 'dvbdump':
            assert r['overlap_mismatches'] == 0 and r['underruns'] == 0, r
        if r['cmd'] == 'dvbpace':
            if command:
                assert r['seconds'] == int(command.split()[3]), r
            assert r['late_fills'] == r['late_switches'] == r['fatal_late'] == 0 and r['min_fill_slack_cycles'] > 0, r
            target = r.get('input_target', 0)*1e6
            capacity = 8*r['payload_bytes_per_frame']*r['symbol_rate']/r['frame_symbols']
            source_limited = r.get('input_loop') and 0 < target < capacity
            if r.get('input_fault') == 6 or source_limited:
                assert r['underruns'] > 0 and r['frames_encoded'] > 0, r
            else:
                assert r['underruns'] == 0, r
            if source_limited and not r.get('input_fault'):
                assert r['input_bytes']*8/r['seconds'] >= .98*target, r
            budget = 240000000*r['frame_symbols']/r['symbol_rate'] if 'frame_symbols' in r else 299538
            assert r['encode_cycles_live_max'] <= budget, r


def decode_dump(path, kind, mc, sps, rate, frames, receiver, replay=False, pv=False):
    import tempfile
    import numpy as np
    import decode
    import dvbs2
    rate_code = dvbs2.RATES[mc-1] if mc < 12 else dvbs2.PSK8_RATES[mc-12]
    per_frame = (dvbs2.code(rate_code).kbch-80)//8
    expected = b''.join(pv_pattern(seq) if pv else pattern(seq % 4 if replay else seq, kind)
                        for seq in range(math.ceil((frames+3)*per_frame/PAYLOAD)))
    data = np.fromfile(path, '<i2').reshape(-1, 2).astype(np.float32)/512
    with tempfile.TemporaryDirectory() as tmp:
        capture = Path(tmp)/'input.cf32'
        (data[:, 0]+1j*data[:, 1]).astype('<c8').tofile(capture)
        report, got = decode.decode(capture, sample_rate=rate, symbol_rate=rate/sps,
                                    receiver=receiver, rrc_rej=20)
    # Host captures show four acquisition frames for 8PSK, two for QPSK.
    required = frames-(4 if mc >= 12 else 2)
    assert report['frame_lock'] and report['lock_at_end'] and report['status_complete'], report
    assert (not report['modcods'] or report['modcods'] == [mc]) and report['bb_frames'] >= required, report
    assert report['stream'] == ('ts' if kind == 1 else 'gcs'), report
    for name in ('ldpc_bit_flips','bch_corrected_bits',
                 'bch_failed_frames','bbheader_rejected_frames'):
        assert report[name] == 0, report
    for name in ('ldpc_checks_before', 'ldpc_checks_after'):
        assert report[name] is None or report[name] == 0, report
    at = expected.find(got)
    assert got and at >= 0, 'decoded input differs from sender pattern'
    assert len(got) >= required*per_frame - (376 if kind == 1 else 0), report
    assert at % (188 if kind == 1 else per_frame) == 0, at
    report.update(input_match=True, input_offset=at, input_bytes=len(got))
    print(json.dumps(report))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check')
    c.add_argument('records', type=Path)
    c.add_argument('--firmware')
    c.add_argument('--folder', type=Path)
    d = sub.add_parser('decode')
    d.add_argument('iq', type=Path)
    d.add_argument('--kind', choices=('ts','gcs'), required=True)
    d.add_argument('--replay', action='store_true')
    d.add_argument('--pv', action='store_true', help='raw TS loopback pattern')
    d.add_argument('--modcod', type=int, default=11)
    d.add_argument('--sps', type=int, default=3)
    d.add_argument('--rate', type=int, default=80000000)
    d.add_argument('--frames', type=int, default=16)
    d.add_argument('--receiver', type=Path, required=True)
    a = p.parse_args()
    if a.cmd == 'check':
        rows = [json.loads(line) for line in a.records.read_text().splitlines()]
        checked = [r for r in rows if not r.get('done') and ('status' in r or r.get('cmd') == 'spirx')]
        failures = []
        if not checked:
            failures.append('no results')
        for r in checked:
            try:
                record_check(r)
                frames = r.get('frames_encoded', 0)
                if frames and r.get('input_cycles'):
                    print(f"{r.get('command', r['cmd'])}: RX active {r['input_cycles']/frames:.0f}, "
                          f"copy {r.get('input_copy_cycles', 0)/frames:.0f}, "
                          f"ISR callback {r['spi']['isr_cycles']/frames:.0f} cycles/frame; "
                          f"encoder live max {r['encode_cycles_live_max']} cycles")
            except (AssertionError, KeyError, ValueError) as e:
                fields = ('status', 'underruns', 'max_fill_cycles', 'min_fill_slack_cycles',
                          'encode_cycles_mean', 'encode_cycles_live_max', 'mbps')
                details = {k:r[k] for k in fields if k in r}
                failures.append(f"{r.get('command', r['cmd'])}: {type(e).__name__} {details}")
        if a.firmware is not None:
            # ESP-IDF esp_app_format/CMakeLists.txt exports PROJECT_VER[:31].
            # Compare the whole exported field, never an arbitrary common prefix.
            expected = a.firmware[:31]
            versions = [r.get('firmware') for r in rows if r.get('cmd') == 'info' and not r.get('done')]
            if not expected or versions != [expected]:
                failures.append(f'wrong or missing firmware: expected {expected!r} '
                                f'(full version {a.firmware!r}), got {versions!r}')
        if a.folder:
            for path in sorted(a.folder.glob('*.iq')):
                data = path.read_bytes()
                crc = zlib.crc32(data)
                matches = [r for r in rows if r.get('crc32') == crc]
                if not matches or not any(r.get('host_crc32') == crc and r.get('binary_bytes') == len(data) for r in rows):
                    failures.append(f'{path.name}: board/USB/file CRC or byte count mismatch')
                expected = {'reg-qpsk.iq':1271511239, 'reg-8psk.iq':3178964763}.get(path.name)
                if expected is not None and crc != expected:
                    failures.append(f'{path.name}: synthetic regression CRC mismatch')
        print(f'{len(checked)} records, {len(failures)} failed gates')
        for failure in failures:
            print('FAIL', failure)
        if failures:
            raise SystemExit(1)
    else:
        decode_dump(a.iq, 1 if a.kind == 'ts' else 2, a.modcod, a.sps, a.rate, a.frames, a.receiver, a.replay, a.pv)


if __name__ == '__main__':
    main()
