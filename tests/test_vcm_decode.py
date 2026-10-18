"""Variable BBFRAME sizes, continuous PRNG and strict VCM interval matching. No devices."""
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
import decode
import dvbs2


def frames(pattern, count, hold=4):
    raw, modes = bytearray(), []
    for mc, data in decode.payload_sequence(pattern, count, 'N0CALL', hold):
        rate = dvbs2.RATES[mc-1] if mc < 12 else dvbs2.PSK8_RATES[mc-12]
        raw.extend(np.packbits(dvbs2.bbframe(np.unpackbits(np.frombuffer(data, np.uint8)),
                                           rate, ccm=0)).tobytes())
        modes.append(mc)
    return decode.parse_bbframes(bytes(raw)), modes


def run():
    for pattern in ((4, 7, 11), (4, 11, 13, 17)):
        rows, modes = frames(pattern, 36)
        options = dict(pattern=pattern, hold_frames=4, max_frames=36, callsign='N0CALL')
        result = decode.vcm_match(rows[3:33], modes[3:33], **options)
        assert result['host_model_exact'] and result['contiguous'] and result['pattern_complete']
        assert (result['first_frame'], result['last_frame']) == (3, 32)
        assert result['compared_bytes'] == sum(len(row['payload']) for row in rows[3:33])
        # PRNG advances through variable capacities, including overwritten ID bytes.
        full = decode.payload(pattern[0], 36, 'N0CALL', pattern=pattern, hold_frames=4)
        assert full == b''.join(row['payload'] for row in rows)
        assert rows[4]['payload'][40:80] != decode.payload(pattern[1], 1, 'N0CALL')[40:80]
        faults = [([], []), (rows, modes[:-1]),
                  (rows[:8] + rows[9:], modes[:8] + modes[9:]),
                  (rows[:8] + rows[7:], modes[:8] + modes[7:]),
                  (rows[:8] + rows[9:10] + rows[8:9] + rows[10:],
                   modes[:8] + modes[9:10] + modes[8:9] + modes[10:])]
        changed = dict(rows[8], payload=rows[8]['payload'][:-1] + bytes((rows[8]['payload'][-1] ^ 1,)))
        faults.append((rows[:8] + [changed] + rows[9:], modes))
        changed = dict(rows[8], header=bytes((rows[8]['header'][0] | 16,)) + rows[8]['header'][1:])
        faults.append((rows[:8] + [changed] + rows[9:], modes))
        for bad_rows, bad_modes in faults:
            assert not decode.vcm_match(bad_rows, bad_modes, **options)['host_model_exact']
        assert not decode.vcm_match(rows, modes, **dict(options, max_frames=35))['contiguous']
        assert not decode.vcm_match(rows, modes, **dict(options, hold_frames=3))['contiguous']
        gap = decode.vcm_match(rows[:8] + rows[9:], modes[:8] + modes[9:], **options)
        assert gap['exact_individual_frames'] == 35 and gap['missing_frames'] == 1
        assert gap['individual_compared_bytes'] == sum(len(f['payload']) for f in rows[:8] + rows[9:])
        assert [(s['first_frame'], s['last_frame']) for s in gap['segments']] == [(0, 7), (9, 35)]
        assert gap['first_mismatch']['reason'] == 'gap' and not gap['host_model_exact']
        assert not decode.vcm_match(rows[:3], modes[:3], **options)['pattern_complete']
    # MODCODs 6 and 13 have identical payload capacities. PLS must still match.
    rows, modes = frames((6, 13), 12)
    assert not decode.vcm_match(rows, [6]*12, pattern=(6, 13), hold_frames=4,
                                max_frames=12, callsign='N0CALL')['host_model_exact']
    status = '\n'.join(f'S2FEC modcod={mc} short=0 pilots=1 ldpc_flips=0 '
                       'ldpc_checks_before=0 ldpc_checks_after=0 bch_errors=0' for mc in modes)
    report = decode.summarize(status, rows, b''.join(row['payload'] for row in rows))
    assert report['frame_modcods'] == modes
    assert report['frames_per_modcod'] == {6: 8, 13: 4}
    failed = ('S2FEC modcod=28 short=1 pilots=0 ldpc_flips=100 '
              'ldpc_checks_before=200 ldpc_checks_after=50 bch_errors=-1\n')
    # Station IDs/acquisition/silence may create failed FEC hypotheses around
    # otherwise exact data. Retain totals and distinguish the recovered interval.
    edge = decode.summarize('FRAMELOCK 1\n' + failed*2 + status + '\n' + failed + 'FRAMELOCK 0\n',
                            rows, b''.join(row['payload'] for row in rows))
    edge.update(decode.vcm_match(rows, modes, pattern=(6, 13), hold_frames=4,
                                 max_frames=12, callsign='N0CALL'))
    assert edge['bch_failed_frames'] == 3 and not edge['lock_at_end']
    assert edge['payload_fec']['failed_before'] == 2 and edge['payload_fec']['failed_after'] == 1
    assert edge['payload_fec']['bch_failed_frames'] == 0
    assert edge['payload_fec']['ldpc_bit_flips'] == 0
    assert all(decode.vcm_checks(edge).values())
    # A failed attempt within that same interval must remain a failure, even
    # when every emitted BBFRAME matches (for example, a false PLS candidate).
    lines = status.splitlines()
    internal = decode.summarize('FRAMELOCK 1\n' + '\n'.join(lines[:4]) + '\n' + failed + '\n'.join(lines[4:]),
                                rows, b''.join(row['payload'] for row in rows))
    internal.update({k: edge[k] for k in ('host_model_exact', 'contiguous', 'pattern_complete')})
    assert internal['payload_fec']['bch_failed_frames'] == 1
    assert not decode.vcm_checks(internal)['payload_fec_valid']
    assert not all(decode.vcm_checks(internal).values())
    assert not decode.vcm_checks(dict(edge, host_model_exact=False))['host_model_exact']
    print('VCM decode: continuous PRNG, BBHEADER/model, PLS order and gap/corruption rejection pass')


if __name__ == '__main__':
    run()
