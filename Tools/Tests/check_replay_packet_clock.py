"""Bounded packet-clock audit for local replay inputs/outputs, not decoder validation."""
import argparse
from decimal import Decimal
import json
from pathlib import Path

from check_long_recording import bounded_process


class PacketClock:
    def __init__(self):
        self.streams = {}

    def add(self, line):
        parts = line.decode('utf-8', errors='strict').strip().split('|')
        if parts == ['']:
            return  # ffprobe MPEG-TS packet side-data formatting emits blank separators.
        if parts[0] != 'packet':
            raise ValueError('Unexpected probe record')
        fields = {}
        for part in parts[1:]:
            if '=' not in part:
                continue
            key, value = part.split('=', 1)
            if key not in ('stream_index', 'dts_time', 'pts_time', 'duration_time'):
                continue
            if key in fields:
                raise ValueError('Duplicate clock field')
            fields[key] = value
        stream = int(fields['stream_index'])
        if not 0 <= stream < 32:
            raise ValueError('Unsupported stream index')
        dts, pts, duration = (Decimal(fields[key]) for key in ('dts_time', 'pts_time', 'duration_time'))
        if not all(value.is_finite() for value in (dts, pts, duration)) or duration <= 0:
            raise ValueError('Invalid or missing packet clock')
        state = self.streams.setdefault(stream, {'packets': 0, 'first': dts, 'previous': None,
            'previousEnd': None, 'lastEnd': None, 'durationSum': Decimal(0),
            'minimumGap': None, 'maximumGap': None, 'regressions': 0, 'duplicates': 0,
            'gapsOver2Ms': 0, 'overlapsOver2Ms': 0, 'largestDiscontinuities': []})
        if state['previous'] is not None:
            step = dts - state['previous']
            state['regressions'] += step < 0
            state['duplicates'] += step == 0
            gap = dts - state['previousEnd']
            state['minimumGap'] = gap if state['minimumGap'] is None else min(gap, state['minimumGap'])
            state['maximumGap'] = gap if state['maximumGap'] is None else max(gap, state['maximumGap'])
            state['gapsOver2Ms'] += gap > Decimal('.002')
            state['overlapsOver2Ms'] += gap < Decimal('-.002')
            if abs(gap) > Decimal('.002'):
                state['largestDiscontinuities'].append({'packet': state['packets'],
                    'dtsSeconds': float(dts), 'gapMs': float(gap * 1000)})
                state['largestDiscontinuities'].sort(key=lambda row: abs(row['gapMs']), reverse=True)
                del state['largestDiscontinuities'][16:]
        state['packets'] += 1
        state['previous'], state['previousEnd'], state['lastEnd'] = dts, dts + duration, dts + duration
        state['durationSum'] += duration

    def report(self):
        return {'streams': {str(stream): {
            'packets': state['packets'], 'firstDtsSeconds': float(state['first']),
            'lastEndSeconds': float(state['lastEnd']),
            'durationSumSeconds': float(state['durationSum']),
            'elapsedMinusDurationSumMs': float((state['lastEnd'] - state['first'] - state['durationSum']) * 1000),
            'minimumGapMs': float(state['minimumGap'] * 1000) if state['minimumGap'] is not None else None,
            'maximumGapMs': float(state['maximumGap'] * 1000) if state['maximumGap'] is not None else None,
            **{key: state[key] for key in ('regressions', 'duplicates', 'gapsOver2Ms', 'overlapsOver2Ms', 'largestDiscontinuities')},
        } for stream, state in sorted(self.streams.items())},
            'limits': 'Packet-clock metadata only, rounded to ffprobe printed precision. Durations may include codec padding/priming; does not measure arrival/network latency, decoder correctness or browser playback.'}


def inspect(path, ffprobe='ffprobe'):
    path = Path(path).resolve(strict=True)
    if not path.is_file() or path.stat().st_size > 8 * 1024**3:
        raise ValueError('Expected local media no larger than 8 GiB')
    clock = PacketClock()
    code, _, nonempty, diagnostics = bounded_process([
        ffprobe, '-v', 'error', '-protocol_whitelist', 'file,pipe',
        '-show_packets', '-show_entries', 'packet=stream_index,pts_time,dts_time,duration_time',
        '-of', 'compact=p=1:nk=0', str(path)], clock.add, timeout=1800)
    if code or nonempty or not clock.streams:
        raise ValueError('Clock probe failed: ' + diagnostics)
    return clock.report()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(inspect(args.input), output, indent=2)
