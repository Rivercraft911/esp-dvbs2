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
    print('VCM decode: continuous PRNG, BBHEADER/model, PLS order and gap/corruption rejection pass')


if __name__ == '__main__':
    run()
