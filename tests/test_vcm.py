"""Mixed-MODCOD firmware lifecycle, host model and optional independent receiver. No ports."""
import argparse
import ctypes as ct
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np

from test_stream import compile_stream
from test_dvbs2_c import NativeEncoder, U8P
from test_ingress import Native, Ring, pv_message, MSG
from test_usb_send import send

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'host'))
import dvbs2 as d
import decode


def input_bytes(n):
    x = 2026
    out = bytearray()
    for _ in range(n):
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        out.append(x & 255)
    return bytes(out)


def test_controls():
    with Native() as native:
        lib = native.lib
        for mc in range(4, 18):
            wire = send.message(bytes((mc,)), 65535, 3)
            assert wire == pv_message(65535, bytes((mc,)), 3)
            assert lib.pv_check(wire, MSG) == 0
        for data in (b'', b'\x03', b'\x12', b'\x04\x05'):
            assert lib.pv_check(pv_message(0, data, 3), MSG) != 0
        ring = Ring(native)
        try:
            ring.send(pv_message(65535, b'\x0d', 3))
            ring.send(pv_message(0, b'payload', 2))
            assert ring.drain() == b'payload'
            assert ring.stats()['control_rejected'] == 1
            assert ring.stats()['msgs'] == 1 and ring.stats()['bytes'] == 7
        finally:
            ring.close()
        ring = Ring(native)
        selected = []
        @ct.CFUNCTYPE(ct.c_bool, ct.c_uint)
        def control(mc):
            selected.append(mc)
            return mc < 12
        lib.sim_control.argtypes = [ct.c_void_p, type(control)]
        lib.sim_control(ring.sim, control)
        try:
            ring.send(pv_message(65535, b'\x0b', 3))
            ring.send(pv_message(65535, b'\x0d', 3))  # duplicate cannot request a change
            bad = bytearray(pv_message(0, b'\x09', 3)); bad[-1] ^= 1
            ring.send(bytes(bad))
            assert ring.drain() == b''
            ring.send(pv_message(0, b'\x0d', 3))  # CRC failure did not consume sequence zero
            ring.send(pv_message(1, b'payload', 2))
            assert ring.drain() == b'payload' and selected == [11, 13]
            stats = ring.stats()
            assert stats['controls'] == stats['control_rejected'] == stats['seq_back'] == stats['err_crc'] == 1
            assert stats['lost'] == 0 and stats['bytes'] == 7
        finally:
            ring.close()


def pattern(sps, hold, axes=False):
    if axes and hold == 3:
        return [4,11,13,17]
    if axes:
        return [4,12,5,13,6,14,7,15,8,16,9,17,10,11]
    return list(range(4,12 if sps == 40 else 18))


def check_model(symbols, payload, sps, hold, axes=False):
    modes = len(pattern(sps,hold,axes))
    count = (32*2+8) if not hold else 2*modes*hold+8
    offset = used = 0
    sequence = []
    with NativeEncoder() as enc:
        for frame in range(count):
            mc = (13 if frame else 4) if not hold else pattern(sps,hold,axes)[(frame//hold)%modes]
            rate = d.RATES[mc-1] if mc < 12 else d.PSK8_RATES[mc-12]
            n = (d.code(rate).kbch-80)//8
            data = payload[used:used+n]
            assert len(data) == n
            bbf = d.bbframe(np.unpackbits(np.frombuffer(data, np.uint8)), rate, ccm=0)
            fec = d.fecframe(bbf, rate)
            expected = d.plframe_phases(fec, rate, pilots=True, modulation='qpsk' if mc < 12 else '8psk')
            np.testing.assert_array_equal(symbols[offset:offset+len(expected)], expected)
            # The actual fast stages reprepare a maximum-size context across
            # BCH t=8/10/12, LDPC rates and both interleavers, as firmware does.
            mode = enc.lib.dvbs2_find_mode(mc)
            assert enc.lib.dvbs2_fast_prepare(enc.fast, mode, 1, 2048|16) == 0
            packed = np.zeros(len(expected)//2, np.uint8)
            raw = np.frombuffer(data, np.uint8)
            assert enc.lib.dvbs2_encode_frame_packed(enc.fast, enc.ws, None, raw.ctypes.data_as(U8P),
                                                     n*8, packed.ctypes.data_as(U8P)) == 0
            phases = np.empty(len(expected), np.uint8)
            phases[::2] = packed & 15; phases[1::2] = packed >> 4
            np.testing.assert_array_equal(phases, expected)
            sequence.append(mc)
            offset += len(expected); used += n
    assert offset == len(symbols) and used == len(payload)
    return sequence


def run(receiver=None, output=None, axes=False):
    test_controls()
    with tempfile.TemporaryDirectory() as tmp:
        folder = output or Path(tmp)
        folder.mkdir(parents=True, exist_ok=True)
        exe = compile_stream(Path(tmp), source='vcm_host.c',
                             defines=('NATIVE_INTERNAL_BUDGET=30000',) if axes else ())
        for sps, hold in (((8,1),(8,2),(8,3),(8,0)) if axes else ((40, 1), (8, 1), (4, 1), (4, 2), (4, 0))):
            stem = folder/f'vcm-n{sps}-hold{hold}'
            symbols = stem.with_suffix('.phases')
            payload = stem.with_suffix('.payload')
            wire = stem.with_suffix('.pv')
            if not hold:
                # Control is consumed during frame zero's assembly. Frame zero
                # stays QPSK; frame one switches to 8PSK without changing bytes.
                data = input_bytes(600000)
                messages = [send.message(b'\x0d', 0, 3)]
                messages += [send.message(data[i:i+1316], 1+i//1316, 2) for i in range(0, len(data), 1316)]
                wire.write_bytes(b''.join(messages))
            result = subprocess.run([str(exe), str(sps), str(hold), str(symbols), str(payload), str(wire)]+(['axes'] if axes else []),
                                    capture_output=True, text=True, timeout=90)
            assert result.returncode == 0, result.stdout[-2000:]+result.stderr
            if axes and hold == 3:
                rows=[json.loads(line[1:]) for line in result.stdout.splitlines() if line.startswith('@{')]
                pace,= [row for row in rows if row.get('cmd') == 'dvbpace' and 'status' in row]
                assert pace['paced_wifi_up'] and pace['vcm_axes'] and pace['status'] == 'ESP_OK'
                assert pace['free_internal'] == 30000 and pace['largest_internal'] == 2048
                assert pace['workspace_bytes'] == 8144 and pace['playback_store_bytes'] == 64192
                assert pace['sample_cache_store_bytes'] == 8704 and pace['table_bytes'] == 10240
                probe,= [row for row in rows if row.get('cmd') == 'wifimem' and 'status' in row]
                assert probe['status'] == 'ESP_OK' and probe['ring_heap_bytes'] == 0
            summary = next(line for line in result.stdout.splitlines() if line.startswith('VCM n'))
            sequence = check_model(np.fromfile(symbols, np.uint8), payload.read_bytes(), sps, hold, axes)
            print(summary+'; every PL symbol and DAC word matches', flush=True)
            if receiver:
                iq = np.fromfile(str(symbols)+'.iq', '<i2').reshape(-1, 2).astype(np.float32)/512
                cf32 = stem.with_suffix('.cf32')
                # Flush receiver lookahead using a copy of the final samples.
                data = (iq[:, 0]+1j*iq[:, 1]).astype('<c8')
                np.concatenate((data, data[-4096:])).tofile(cf32)
                report, got = decode.decode(cf32, sample_rate=4000000 if sps == 40 else 40000000,
                    symbol_rate=40000000/sps, receiver=receiver, log=stem.with_suffix('.log'))
                assert report['frame_lock'] and report['lock_at_end'] and report['status_complete'], report
                assert set(report['modcods']) == set(sequence), report
                assert got and got in payload.read_bytes(), report
                assert report['bb_frames'] >= (32 if not hold else 2*len(pattern(sps,hold,axes))*hold)-5, report
                for key in ('ldpc_bit_flips', 'bch_corrected_bits', 'bch_failed_frames', 'bbheader_rejected_frames'):
                    assert report[key] == 0, report
                print(json.dumps(dict(sps=sps, hold=hold, frames=report['bb_frames'], modcods=report['modcods'],
                                      recovered_bytes=len(got), contiguous=True)), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receiver', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--axes', action='store_true')
    args = parser.parse_args()
    run(args.receiver, args.output, args.axes)
