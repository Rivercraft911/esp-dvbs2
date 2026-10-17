"""Coordinator gates reject corrupted, slow and late records. No serial access."""
import copy
import pathlib
import sys
import zlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'host'))
from spi_input import pattern, record_check


def rejected(r):
    try:
        record_check(r)
    except AssertionError:
        return
    raise AssertionError('bad record passed')


def main():
    spi = dict(msgs=2, bytes=2632, nops=0, short=0, long=0, header=0, crc=0, sync=0,
               lost=0, back=0, overruns=0, restarts=0, ready_falls=0, wrong_type=0)
    r = dict(cmd='spirx', command='spirx 3000 gcs loop 87', input='gcs', spi=spi,
             verified_msgs=2, verified_bytes=2632, bad_payload=0, mbps=87,
             crc32=zlib.crc32(pattern(0, 2)+pattern(1, 2)))
    record_check(r)
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
    demo['encode_cycles_live_max'] += 1; rejected(demo)
    demo.update(encode_cycles_live_max=532656, input_copy_cycles=2, input_cycles=1)
    rejected(demo)
    print('SPI run gates: corruption, CRC, loss, throughput, starvation and timing: ok')


if __name__ == '__main__':
    main()
