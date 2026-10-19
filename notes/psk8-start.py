#!/usr/bin/env python3
"""Coordinator only: cold-reset, radio-off startup checks; append raw evidence."""
import argparse
import datetime as dt
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
import board


def summary(record):
    trace = record.get('startup', {})
    columns = trace.get('columns', [])
    banks = [dict(zip(columns, values)) for values in trace.get('banks', [])]
    cycles = [b['fill_cycles'] for b in banks]
    peak = max(cycles, default=0)
    return dict(first=cycles[0] if cycles else None, peak32=peak,
                peak_bank=cycles.index(peak) if cycles else None,
                max_fill=record.get('max_fill_cycles'),
                late=record.get('late_fills'), fatal=record.get('fatal_late'),
                trace_max=max((b.get('trace_cycles', 0) for b in banks), default=0),
                psram32=sum(b.get('psram_miss', 0) for b in banks),
                wifi_min=record.get('wifi_min_free'),
                status=record.get('status'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--port', default=board.DEFAULT_PORT)
    p.add_argument('--out', type=Path, default=ROOT / 'notes/evidence/psk8-start-fixed.jsonl')
    a = p.parse_args()
    if not 1 <= a.repeats <= 100:
        p.error('--repeats must be 1..100')
    port = board.resolve_port(a.port)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    failed = False
    with board.reserve_port(port, reset=True), a.out.open('a') as evidence:
        for repeat in range(1, a.repeats + 1):
            for mc in (13, 15, 17):
                command = f'dvbpace {mc} 26m667 10'
                try:
                    records = board.run(port, command, timeout=40, reset=True)
                    stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds')
                    for record in records:
                        evidence.write(json.dumps(dict(host_time=stamp, command=command,
                                                       repeat=repeat, **record)) + '\n')
                    evidence.flush()
                    record, = [r for r in records if r.get('cmd') == 'dvbpace' and 'status' in r]
                    row = summary(record)
                    print(f'mc={mc} run={repeat} ' + ' '.join(f'{k}={v}' for k, v in row.items()), flush=True)
                    failed |= (row['status'] != 'ESP_OK' or row['late'] != 0 or row['fatal'] != 0 or
                               row['max_fill'] is None or row['max_fill'] > 43919 or
                               len(record.get('startup', {}).get('banks', [])) != 32 or
                               row['wifi_min'] is None or row['wifi_min'] < 20000)
                except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
                    failed = True
                    evidence.write(json.dumps(dict(command=command, repeat=repeat, host_error=str(exc))) + '\n')
                    evidence.flush()
                    print(f'mc={mc} run={repeat} ERROR {exc}', flush=True)
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
