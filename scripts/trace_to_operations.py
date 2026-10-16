#!/usr/bin/env python3
"""Normalize observed JNI pairs; preserve ordering and native lifecycle evidence.
This emits reconstruction INPUT, never inferred bytecode or arithmetic provenance.
"""
import argparse
import json
from pathlib import Path
from expand_trace import expand, open_text


def operations(records):
    pending = {}
    last = 0
    for order, record in enumerate(expand(records), 1):
        last = order
        ev = record.get('ev')
        if ev == 'enter':
            seq = record['seq']
            if seq in pending:
                raise ValueError(f'duplicate JNI enter {seq}')
            pending[seq] = (order, record)
        elif ev == 'exit':
            seq = record['seq']
            if seq not in pending:
                raise ValueError(f'exit without enter {seq}')
            start, entry = pending.pop(seq)
            yield {'ev': 'operation', 'schema': 2, 'id': seq,
                   'start_order': start, 'end_order': order,
                   'input': entry, 'output': record,
                   'status': 'exception' if record.get('exception') else 'returned',
                   'provenance': 'observed-only'}
        else:
            yield {**record, 'order': order}
    for seq, (start, entry) in pending.items():
        yield {'ev': 'operation', 'schema': 2, 'id': seq,
               'start_order': start, 'end_order': None, 'observed_until': last,
               'input': entry, 'output': None, 'status': 'incomplete',
               'provenance': 'observed-only'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error('input and output must differ')
    with open_text(args.input, 'rt') as src, open_text(args.output, 'wt') as dst:
        for op in operations(json.loads(line) for line in src if line.strip()):
            dst.write(json.dumps(op, ensure_ascii=True, allow_nan=False, separators=(',', ':')) + '\n')


if __name__ == '__main__':
    main()
