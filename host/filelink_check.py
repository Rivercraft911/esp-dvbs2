#!/usr/bin/env python3
"""Offline FEC proof from a coordinator's radio-off QPSK 1/2 USB IQ dump. No ports."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import random
import tempfile
import zlib

import filelink

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_file_sender', ROOT/'examples/usb-send/send.py')
sender = importlib.util.module_from_spec(spec); spec.loader.exec_module(sender)


def loss_cases(raw, seed=20261018):
    """Remove exactly m whole packets in each complete block, including packet 0."""
    packets = list(filelink.packets_from_stream(raw))
    filelink.recover_packets(packets)  # validate identities, sequences and hash first
    groups = {}
    for packet in packets:
        _, _, _, seq, k, m, _ = filelink.HEADER.unpack_from(packet)
        groups.setdefault(seq // (k+m), {})[seq % (k+m)] = packet
    rng = random.Random(seed)
    cases = {'clean': b''.join(packets), 'random': [], 'burst': [], 'over-budget': []}
    for block, received in sorted(groups.items()):
        if set(received) != set(range(k+m)):
            raise ValueError(f'proof requires all {k+m} packets before injection in block {block}')
        random_loss = {0} | set(rng.sample(range(1, k+m), m-1))
        start = rng.randrange(k+1)
        for name, lost in (('random', random_loss), ('burst', set(range(start, start+m))),
                           ('over-budget', set(range(m+1)))):
            cases[name].extend(packet for index, packet in sorted(received.items()) if index not in lost)
    return {name: value if isinstance(value, bytes) else b''.join(value) for name, value in cases.items()}


def verify(raw, original, folder):
    """Exercise the same unpack path as the CLI, and require no partial output."""
    results = {}
    for name, payload in loss_cases(raw).items():
        output = Path(folder)/(name+'.bin')
        if name == 'over-budget':
            try:
                sender.unpack(io.BytesIO(payload), output, fec=True)
            except ValueError:
                if output.exists():
                    raise ValueError('failed recovery published output')
                results[name] = 'rejected; no output'
            else:
                raise ValueError('over-budget erasures unexpectedly recovered')
        else:
            result = sender.unpack(io.BytesIO(payload), output, fec=True)
            if output.read_bytes() != original:
                raise ValueError(f'{name}: recovered file differs')
            results[name] = result['sha256']
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('original', type=Path)
    p.add_argument('iq', type=Path)
    p.add_argument('report', type=Path)
    p.add_argument('--receiver', default=os.environ.get('RX', 'leandvb'))
    a = p.parse_args()
    import numpy as np
    import decode
    report = json.loads(a.report.read_text())
    if report['errors'] or report['command'].split()[:2] != ['dvbdump', '4'] or len(report['command'].split()) != 3:
        p.error('need a successful radio-off MODCOD 4, 1m dump report')
    sender.check_run(report['sender'], report['records'], dump=True, require_eof=True)
    if not report['sender']['complete'] or report['sender']['binary_remaining']:
        p.error('incomplete dump session')
    wire = a.iq.read_bytes()
    stats = report['sender']
    if len(wire) % 4 or len(wire) != stats['binary_bytes'] or zlib.crc32(wire) != stats['host_crc32']:
        p.error('IQ file length/CRC differs from USB report')
    if report.get('fec', {}).get('frame_bytes') != 4016:
        p.error('missing FEC parameters or wrong capacity')
    with tempfile.TemporaryDirectory(prefix='filelink-dump-') as folder:
        samples = np.frombuffer(wire, '<i2').reshape(-1, 2).astype(np.float32)/512
        capture = Path(folder)/'dump.cf32'
        (samples[:, 0]+1j*samples[:, 1]).astype('<c8').tofile(capture)
        rx, raw = decode.decode(capture, sample_rate=4e6, symbol_rate=1e6,
                                receiver=a.receiver, timeout=120)
        if not all(rx[key] for key in ('frame_lock', 'lock_at_end', 'status_complete')) or rx['stream'] != 'gcs':
            raise ValueError(f'LeanDVB did not lock on GCS: {rx}')
        if any(rx[key] != 0 for key in ('ldpc_bit_flips', 'bch_corrected_bits', 'bch_failed_frames', 'bbheader_rejected_frames')):
            raise ValueError(f'clean dump had FEC errors: {rx}')
        checks = verify(raw, a.original.read_bytes(), folder)
    print(json.dumps(dict(iq_sha256=hashlib.sha256(wire).hexdigest(), fec=report['fec'], checks=checks)))


if __name__ == '__main__':
    main()
