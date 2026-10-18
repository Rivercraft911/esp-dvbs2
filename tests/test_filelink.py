"""File FEC, both sender CLIs with fake devices, and optional independent IQ proof. No ports."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import itertools
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'host'))
import filelink as f
import filelink_check as proof

send = proof.sender


def rejects(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except ValueError:
        return
    raise AssertionError('invalid transfer accepted')


def crc(packet):
    return bytes(packet[:-4])+struct.pack('>I', zlib.crc32(packet[:-4]))


def test_code():
    # Independent polynomial multiplication, no logarithms/tables.
    def multiply(a, b):
        value = 0
        for _ in range(8):
            if b & 1: value ^= a
            b >>= 1; a <<= 1
            if a & 256: a ^= 0x11d
        return value
    for a in range(256):
        for b in range(256):
            assert f.mul(a, b) == multiply(a, b)
        if a: assert multiply(a, f.inv(a)) == 1
    # A fixed, independently calculated Cauchy vector: source 01,02; repair 7b,f5.
    packets = list(f.encode_packets(b'\x01\x02', 2, 2, 1))
    assert [p[f.HEADER.size:-4].hex() for p in packets] == ['01', '02', '7b', 'f5']
    for length in (0, 1, 79, 200):
        data = bytes(range(length))
        packets = list(f.encode_packets(data, 4, 3, 22))
        for lost in itertools.combinations(range(7), 3):
            keep = [p for i, p in enumerate(packets) if i % 7 not in lost]
            assert f.recover_packets(keep[::-1]+keep[:1]) == data
    rng = random.Random(20261018)
    for k, m in ((16, 4), (1, 255), (255, 1), (8, 8)):
        data = rng.randbytes(k*17+31)
        packets = list(f.encode_packets(data, k, m, 17))
        keep = []
        for at in range(0, len(packets), k+m):
            lost = set(rng.sample(range(k+m), m))
            keep.extend(p for i, p in enumerate(packets[at:at+k+m]) if i not in lost)
        rng.shuffle(keep)
        assert f.recover_packets(keep) == data
    for value in ('0:4', '16:0', '256:1', '255:2', 'a:b', '16', '1:2:3'):
        rejects(f.parameters, value)
    assert f.parameters('16:4') == (16, 4)
    print('GF(256): independent polynomial arithmetic, Cauchy vector, exhaustive 4+3 losses, 256-symbol limits: ok')


def test_integrity():
    data = bytes(range(256))*2
    packets = list(f.encode_packets(data, 4, 2, 80))
    assert f.recover(b'noise'+b''.join(packets)+b'PFL1') == data
    corrupt = bytearray(packets[0]); corrupt[60] ^= 1
    assert f.recover(b''.join([bytes(corrupt), *packets[1:]])) == data
    rejects(f.recover, b''.join(packets[3:]))  # 3 losses in first block
    rejects(f.recover, b''.join(packets[:6]))  # missing entire second block
    rejects(f.recover, b'PFL1'+bytes(60))
    rejects(f.recover_packets, [*packets, *f.encode_packets(b'other', 4, 2, 80)])
    conflict = bytearray(packets[0]); conflict[f.HEADER.size] ^= 1
    rejects(f.recover_packets, [*packets, crc(conflict)])
    # CRC-valid but wrong content must still fail the file hash.
    rejects(f.recover_packets, [crc(conflict), *[p for i, p in enumerate(packets[1:], 1) if i % 6 < 4]])
    outside = bytearray(packets[0]); struct.pack_into('>I', outside, 44, 999)
    rejects(f.recover_packets, [crc(outside)])
    # Malicious basename remains protected by the original container's policy.
    evil = send.HEADER.pack(send.MAGIC, 6, 0, hashlib.sha256(b'').digest())+b'../bad'
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)/'output'
        rejects(send.unpack, io.BytesIO(b''.join(f.encode_packets(evil, 4, 2, 80))), out, fec=True)
        rejects(send.unpack, io.BytesIO(b''.join(packets[3:])), out, fec=True)
        assert not out.exists()
    # Public --bbf handling preserves frame boundaries and validates BBHEADER CRC.
    import decode
    with send.container(io.BytesIO(data), 'test.bin', 4016, (4, 2)) as source:
        raw = source.read()
    records = []
    for at in range(0, len(raw), 4016):
        header = bytes.fromhex('70000000')+struct.pack('>H', 4016*8)+bytes(3)
        records.append(header+bytes([decode.crc8(header)])+raw[at:at+4016])
    bbf = b''.join(records)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)/'bbf.bin'
        send.unpack(io.BytesIO(bbf), out, fec=True, bbf=True)
        assert out.read_bytes() == data
        out.unlink()
        rejects(send.unpack, io.BytesIO(bbf[:-1]), out, fec=True, bbf=True)
        bad = bytearray(bbf); bad[9] ^= 1
        rejects(send.unpack, io.BytesIO(bad), out, fec=True, bbf=True)
        assert not out.exists()
    print('integrity: CRC erasures/resync, missing blocks, over-budget, mixed identities, duplicates, sequence, SHA and paths: ok')


def test_modes_and_transport():
    import dvbs2
    spec = importlib.util.spec_from_file_location('pi_file_sender', ROOT/'examples/pi-sender/send.py')
    pi = importlib.util.module_from_spec(spec); spec.loader.exec_module(pi)
    # Check every supported MODCOD against the reference's actual Kbch tables.
    for mc in range(4, 18):
        capacity, _ = send.mode(mc, '5m')
        rate = dvbs2.RATES[mc-1] if mc < 12 else dvbs2.PSK8_RATES[mc-12]
        assert capacity == (dvbs2.code(rate).kbch-80)//8
        data = bytes(range(256))*17
        with send.container(io.BytesIO(data), 'test.bin', capacity, (4, 2)) as source:
            encoded = source.read()
        assert len(encoded) % capacity == 0
        packets = [encoded[i:i+capacity] for i in range(8*capacity, len(encoded)-4*capacity, capacity)]
        assert all(len(p) == capacity and p.startswith(f.MAGIC) for p in packets)
        # Both transport packetizers may split shards, but reconstruct the exact
        # frame-aligned byte stream from declared PV payload lengths, no PV padding.
        for module in (pi, send):
            wire = [module.message(p, i, 2) for i, p in enumerate(module.payloads(io.BytesIO(encoded), 2))]
            got = b''.join(p[12:12+int.from_bytes(p[6:8], 'little')] for p in wire)
            assert got == encoded
        with tempfile.TemporaryDirectory() as tmp:
            assert proof.verify(encoded, data, tmp)['random']
    # Empty named files, very short files, padding-only source shards.
    for data in (b'', b'x'):
        with send.container(io.BytesIO(data), 'empty.bin', 4016, (16, 4)) as source, tempfile.TemporaryDirectory() as tmp:
            proof.verify(source.read(), data, tmp)
    print('all 14 MODCOD capacities; one shard/frame; USB/SPI segmentation; named/empty containers: ok')


def test_clis():
    from test_usb_send import Peer
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp); original = folder/'input.ts'; original.write_bytes(bytes(range(256))*19)
        class RecordingPeer(Peer):
            def __init__(self):
                super().__init__(); self.confirmed = bytearray()
            def write(self, data):
                n = super().write(data)
                self.confirmed.extend(data[:n])
                return n
        peer = RecordingPeer()
        # Observe the real USB transport producer's payload, with the existing
        # short-write/fragmented-read fake peer and an actual CLI invocation.
        def collect(link, cmd, timeout=10, records=None):
            rows = [dict(cmd=cmd.split()[0], firmware='fake', callsign='')]
            if records is not None: records.extend(rows)
            return rows
        with patch.object(send, 'open_port', return_value=peer), patch.object(send, 'collect', side_effect=collect), \
                contextlib.redirect_stderr(io.StringIO()):
            send.main(['send', str(original), '--fec', '4:2', '--report', str(folder/'usb.json')])
        wire = peer.confirmed.partition(b'\n')[2]
        raw = b''.join(wire[i+12:i+12+int.from_bytes(wire[i+6:i+8], 'little')]
                       for i in range(0, len(wire), 1332))
        with tempfile.TemporaryDirectory() as check:
            proof.verify(raw, original.read_bytes(), check)
        assert json.loads((folder/'usb.json').read_text())['fec']['k'] == 4
        decoded = folder/'decoded.gcs'; decoded.write_bytes(proof.loss_cases(raw)['burst'])
        output = folder/'cli.bin'
        subprocess.run([sys.executable, str(ROOT/'examples/usb-send/send.py'), 'unpack', str(decoded),
                        '--fec', '-o', str(output)], check=True, capture_output=True)
        assert output.read_bytes() == original.read_bytes()
        result = subprocess.run([sys.executable, str(ROOT/'examples/usb-send/send.py'), 'unpack', str(decoded),
                                 '--fec', '-o', str(output)], capture_output=True)
        assert result.returncode != 0  # no overwrite
        # Offline board-dump checker with fake USB evidence and a fake decoder.
        # Validate its real counter/CRC gates; no serial or radio is available.
        import decode
        dump_peer = Peer(dump=True)
        dump_peer.iq = bytes(range(256))*4
        stats, rows = send.stream(dump_peer, io.BytesIO(raw), cmd='dvbdump 4 48',
                                  kind=2, seconds=10, dump=True)
        report = dict(command='dvbdump 4 48', sender=stats, records=rows, errors=[],
                      fec=dict(k=4, m=2, frame_bytes=4016))
        dump = folder/'fake.iq'; dump.write_bytes(dump_peer.iq)
        evidence = folder/'fake.json'; evidence.write_text(json.dumps(report))
        rx = dict(frame_lock=True, lock_at_end=True, status_complete=True, stream='gcs',
                  ldpc_bit_flips=0, bch_corrected_bits=0, bch_failed_frames=0, bbheader_rejected_frames=0)
        args = ['filelink_check.py', str(original), str(dump), str(evidence)]
        with patch.object(decode, 'decode', return_value=(rx, raw)), patch.object(sys, 'argv', args), \
                contextlib.redirect_stdout(io.StringIO()) as text:
            proof.main()
        assert json.loads(text.getvalue())['checks']['over-budget'] == 'rejected; no output'
        dump.write_bytes(dump_peer.iq+b'bad!')
        with patch.object(decode, 'decode', side_effect=AssertionError('decoded an invalid dump')), \
                patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
            try: proof.main()
            except SystemExit as exc: assert exc.code == 2
            else: raise AssertionError('bad dump CRC accepted')
        # Exercise SPI main through fake gpiod/spidev modules. No real imports.
        wires = []
        class SPI:
            def __init__(self, *args): pass
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def xfer2(self, data): wires.append(bytes(data)); return bytes(len(data))
        class GPIO(SPI):
            def get_value(self, pin): return 1
        spec = importlib.util.spec_from_file_location('pi_file_sender', ROOT/'examples/pi-sender/send.py')
        pi = importlib.util.module_from_spec(spec); spec.loader.exec_module(pi)
        modules = {'gpiod': types.SimpleNamespace(request_lines=lambda *a, **kw: GPIO(), LineSettings=lambda **kw: None),
                   'gpiod.line': types.SimpleNamespace(Direction=types.SimpleNamespace(INPUT=0), Value=types.SimpleNamespace(ACTIVE=1)),
                   'spidev': types.SimpleNamespace(SpiDev=SPI)}
        with patch.dict(sys.modules, modules), patch.object(sys, 'argv', ['send.py', str(original), '--fec', '4:2',
                '--modcod', '4', '--mbps', '20']), contextlib.redirect_stderr(io.StringIO()):
            pi.main()
        spi_raw = b''.join(p[12:12+int.from_bytes(p[6:8], 'little')] for p in wires)
        assert spi_raw == raw
        # Default --fec is 16:4, incompatible kinds and undersized dumps fail
        # before a serial connection could be opened.
        with patch.object(send, 'open_port', side_effect=AssertionError('opened a port')), contextlib.redirect_stderr(io.StringIO()):
            for extra in (['--fec', '--kind', 'ts'], ['--fec', '--dump', str(folder/'small.iq')]):
                try: send.main(['send', str(original), '--report', str(folder/'bad.json'), *extra])
                except SystemExit as exc: assert exc.code == 1
                else: raise AssertionError('invalid FEC send accepted')
        assert not (folder/'small.iq').exists()
    print('actual USB send/unpack and SPI CLI: fake transports, hashes, FEC defaults, dump preflight, no overwrite: ok')


def test_iq(receiver):
    import numpy as np
    from scipy.signal import upfirdn
    import decode
    import dvbs2 as d
    # Separate RRC formula (4 samples/symbol), not the firmware shaper.
    t = np.arange(-24, 25, dtype=float)/4
    alpha = .35
    h = np.array([1+alpha*(4/np.pi-1) if x == 0 else
                  (np.sin(np.pi*x*(1-alpha))+4*alpha*x*np.cos(np.pi*x*(1+alpha)))/
                  (np.pi*x*(1-(4*alpha*x)**2)) for x in t])
    for mc, rate, modulation in ((4, '1/2', 'qpsk'), (13, '2/3', '8psk')):
        capacity, _ = send.mode(mc, '5m')
        data = random.Random(mc).randbytes(16*(capacity-f.OVERHEAD)+137)
        with send.container(io.BytesIO(data), 'proof.bin', capacity, (16, 4)) as source:
            raw = source.read()
        phases = []
        for at in range(0, len(raw), capacity):
            bits = np.unpackbits(np.frombuffer(raw[at:at+capacity], np.uint8))
            fec = d.fecframe(d.bbframe(bits, rate), rate)
            phases.append(d.plframe_phases(fec, rate, modulation=modulation))
        rng = random.Random(20261018)
        # Destroy coded data in selected complete frames, retaining PL headers
        # and pilots so the receiver keeps timing. BCH must erase these frames.
        # This models frame erasures, not reacquisition after carrier blackouts.
        schedules = {'clean': [], 'random': [], 'burst': []}
        for block in range(2):
            start = 8+block*20
            schedules['random'].extend(start+i for i in rng.sample(range(20), 4))
            schedules['burst'].extend(range(start+5, start+9))
        for name, lost in schedules.items():
            damaged = list(phases)
            noise = np.random.default_rng(20261018)
            for index in lost:
                random_fec = noise.integers(0, 2, d.code(rate).n, dtype=np.uint8)
                damaged[index] = d.plframe_phases(random_fec, rate, modulation=modulation)
            symbols = np.exp(1j*np.pi/4*np.concatenate(damaged))
            iq = upfirdn(h, symbols, up=4).astype('<c8')
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)/'model.cf32'; iq.tofile(path)
                rx, got = decode.decode(path, sample_rate=4e6, symbol_rate=1e6,
                                        receiver=receiver, timeout=120)
                assert rx['frame_lock'] and rx['lock_at_end'] and rx['stream'] == 'gcs', rx
                packets = list(f.packets_from_stream(got))
                counts = [0, 0]
                for packet in packets:
                    counts[f.HEADER.unpack_from(packet)[3]//20] += 1
                assert counts == ([20, 20] if name == 'clean' else [16, 16]), (name, counts, rx)
                assert rx['bch_failed_frames'] == len(lost), rx
                result = send.unpack(io.BytesIO(got), Path(tmp)/'iq.bin', fec=True)
                assert (Path(tmp)/'iq.bin').read_bytes() == data
                if name == 'clean':
                    # Also prove the exact m=4 bound on decoded whole-frame loss,
                    # independent of the receiver's acquisition behavior.
                    proof.verify(got, data, Path(tmp))
                print(f'MODCOD {mc} model IQ/{name} -> LeanDVB -> unpack: '
                      f'{len(lost)} erased frames, losses/block {[20-n for n in counts]}, '
                      f'{len(data)} bytes, SHA-256 {result["sha256"]}', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receiver', type=Path)
    a = p.parse_args()
    test_code(); test_integrity(); test_modes_and_transport(); test_clis()
    if a.receiver:
        test_iq(a.receiver)
