"""Coordinator gates reject corrupted, slow and late records. No serial access."""
import copy
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
from spi_input import pattern, pv_pattern, record_check, encoder_check


def rejected(r):
    try:
        record_check(r)
    except AssertionError:
        return
    raise AssertionError('bad record passed')


def test_firmware(record):
    # Actual IDF v6.1 truncation from the coordinator's 6720605 run.
    full = 'archive/dev-2026-10-17-1-g6720605'
    cut = 'archive/dev-2026-10-17-1-g67206'
    with tempfile.TemporaryDirectory() as tmp:
        records = pathlib.Path(tmp)/'runs.jsonl'
        for expected, infos, passes in (
                (full, [dict(cmd='info', firmware=cut)], True),
                ('6720605-dirty', [dict(cmd='info', firmware='6720605-dirty')], True),
                ('6720605', [dict(cmd='info', firmware='6720605-dirty')], False),
                (full, [dict(cmd='info', firmware=cut[:-1])], False),
                (full, [dict(cmd='info', firmware=cut+'0')], False),
                (full, [dict(cmd='info', firmware='archive/dev-2026-10-17-1-g12345')], False),
                (full, [], False),
                (full, [dict(cmd='fecbench', firmware=cut)], False),
                (full, [dict(cmd='info', firmware=cut, done=True)], False),
                (full, [dict(cmd='info', firmware=cut), dict(cmd='info', firmware='stale')], False),
                ('', [dict(cmd='info', firmware=cut)], False),
                ('', [dict(cmd='info', firmware='')], False)):
            records.write_text(''.join(json.dumps(r)+'\n' for r in [record, *infos]))
            run = subprocess.run([sys.executable, str(ROOT/'host/spi_input.py'), 'check',
                                  str(records), '--firmware', expected],
                                 capture_output=True, text=True, timeout=10)
            assert run.returncode == (0 if passes else 1), run.stdout+run.stderr
            assert ('wrong or missing firmware' in run.stdout) != passes, run.stdout
    print('SPI firmware gate: IDF truncation, exact fields, dirty/stale/missing/mixed info: ok')


def main():
    spi = dict(msgs=2, bytes=2632, nops=0, short=0, long=0, header=0, crc=0, sync=0,
               lost=0, back=0, overruns=0, restarts=0, ready_falls=0, wrong_type=0)
    r = dict(cmd='spirx', command='spirx 3000 gcs loop 87', input='gcs', spi=spi,
             verified_msgs=2, verified_bytes=2632, bad_payload=0, mbps=87,
             crc32=zlib.crc32(pattern(0, 2)+pattern(1, 2)))
    record_check(r)
    pv = copy.deepcopy(r)
    pv.update(command='spirx 3000 pv loop 9', input='ts', input_wire='pv', input_loop=True,
              mbps=9, crc32=zlib.crc32(pv_pattern(0)+pv_pattern(1)))
    chain = 0
    for seq in range(2):
        body = b'PV\x01\x01'+struct.pack('<HHI', seq, 1316, 0)+pv_pattern(seq)
        chain = zlib.crc32(struct.pack('<I', zlib.crc32(body)), chain)
    pv['spi'].update(messages=2, ts_packets=14, payload_bytes=2632, crc_chain=chain, next_seq=2)
    record_check(pv)
    test_firmware(pv)
    for key in ('messages','ts_packets','payload_bytes','crc_chain','next_seq'):
        bad = copy.deepcopy(pv); bad['spi'][key] += 1; rejected(bad)
    for key, value in (('crc32', 0), ('bad_payload', 1), ('mbps', 80), ('verified_msgs', 0)):
        bad = copy.deepcopy(r); bad[key] = value; rejected(bad)
    capacity = copy.deepcopy(r)
    capacity.update(command='spirx 1000 gcs loop 0', mbps=57.65)
    record_check(capacity) # saturation reports an achieved rate, without a target-rate claim
    capacity.update(command='spirx 1000 gcs loop 46.6', mbps=45)
    rejected(capacity)
    for key in ('short', 'crc', 'lost', 'overruns', 'back', 'wrong_type'):
        bad = copy.deepcopy(r); bad['spi'][key] = 1; rejected(bad)
    pace = dict(cmd='dvbpace', command='dvbpace 11 26m667 30', status='ESP_OK',
                encoder_failed=False, input='gcs', spi=spi, input_bytes=2632,
                frames_encoded=10, seconds=30, late_fills=0, late_switches=0, fatal_late=0,
                min_fill_slack_cycles=7083, max_fill_cycles=40137, underruns=0,
                sps=3, encode_cycles_mean=254607, encode_cycles_live_max=299538)
    record_check(pace)
    for key, value in (('seconds', 2), ('underruns', 1), ('late_fills', 1),
                       ('late_switches', 1), ('max_fill_cycles', 40138),
                       ('min_fill_slack_cycles', 7082), ('encode_cycles_mean', 254608),
                       ('encode_cycles_live_max', 299539)):
        bad = copy.deepcopy(pace); bad[key] = value; rejected(bad)
    pace['input_fault'] = 6
    rejected(pace)
    pace['underruns'] = 1
    record_check(pace)
    wifi = dict(cmd='wifimem', status='ESP_OK', encoder_failed=False, input='ts',
                spi=spi, frames_encoded=0, input_bytes=0, input_cycles=0,
                frames_played=0, positions=0, max_fill_call_cycles=0,
                wifi_free_up=20000, wifi_min_free=20000)
    record_check(wifi)  # receive-only heap probe does not need an external source
    for key in ('wifi_free_up', 'wifi_min_free'):
        bad = copy.deepcopy(wifi); bad[key] = 19999; rejected(bad)
    demo = copy.deepcopy(pace)
    demo.update(command='dvbpace 13 10m 30', input_fault=0, underruns=0, sps=4,
                frame_symbols=22194, symbol_rate=10000000, encode_cycles_live_max=532656)
    record_check(demo)
    pvpace = dict(demo, input_wire='pv', input_loop=True, input_target=9, spi=pv['spi'])
    rejected(pvpace)  # a 9 Mb/s source cannot fill this 19.36 Mb/s mode
    pvpace['underruns'] = 1
    record_check(pvpace)
    demo['encode_cycles_live_max'] += 1; rejected(demo)
    demo.update(encode_cycles_live_max=532656, input_copy_cycles=2, input_cycles=1)
    rejected(demo)
    base = dict(cmd='dvbpace', modcod=13, symbol_rate=10000000, status='ESP_OK',
                input='synthetic', seconds=30, encode_cycles_mean=167000, bch_variant='resident-byte')
    rows = [base]
    for kind in ('ts', 'gcs'):
        for loop in (True, False):
            rows.append(dict(base, input=kind, input_loop=loop, input_replay=not loop,
                             frames_encoded=100, input_copy_cycles=400000, encode_cycles_mean=179350))
    encoder_check(rows) # exact 1.05 * 167000 + 4000 boundary
    for bad in (rows[:-1], [dict(r, encode_cycles_mean=179351) if i else r for i,r in enumerate(rows)],
                [dict(r, bch_variant='nibble') if i else r for i,r in enumerate(rows)]):
        try: encoder_check(bad)
        except AssertionError: pass
        else: raise AssertionError('bad encoder comparison passed')
    print('SPI run gates: corruption, CRC, loss, throughput, starvation and timing: ok')


if __name__ == '__main__':
    main()
