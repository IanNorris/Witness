"""Read-only fMP4 packet/fragment timing audit; not a browser playback verdict."""
import argparse
from bisect import bisect_right
from decimal import Decimal
import json
from pathlib import Path
import struct

from check_long_recording import bounded_process


def media_extents(path):
    """Associate top-level mdat payloads with their preceding moof, without loading media."""
    size = path.stat().st_size
    if not 0 < size <= 8 * 1024**3:
        raise ValueError('Expected local fMP4 no larger than 8 GiB')
    extents, fragment, offset, boxes = [], None, 0, 0
    with path.open('rb') as source:
        while offset < size:
            boxes += 1
            if boxes > 100000 or size - offset < 8:
                raise ValueError('Invalid or excessive top-level boxes')
            source.seek(offset)
            length, kind = struct.unpack('>I4s', source.read(8))
            header = 8
            if length == 1:
                if size - offset < 16:
                    raise ValueError('Truncated extended box')
                length = struct.unpack('>Q', source.read(8))[0]
                header = 16
            elif length == 0:
                length = size - offset
            if length < header or length > size - offset:
                raise ValueError('Invalid box extent')
            if kind == b'moof':
                fragment = 0 if fragment is None else fragment + 1
            if kind == b'mdat':
                if fragment is None:
                    raise ValueError('Not fragmented media: mdat precedes moof')
                extents.append((offset + header, offset + length, fragment))
            offset += length
    if not extents:
        raise ValueError('No fragmented media payloads')
    return extents


class FragmentTiming:
    def __init__(self, extents):
        self.extents = extents
        self.starts = [item[0] for item in extents]
        self.fragments = {}
        self.packets = 0

    def add(self, line):
        parts = line.decode('utf-8', errors='strict').strip().split('|')
        if parts[0] != 'packet':
            raise ValueError('Unexpected packet record')
        fields = {}
        for part in parts[1:]:
            key, value = part.split('=', 1)
            if key in fields:
                raise ValueError('Duplicate packet field')
            fields[key] = value
        position, stream = int(fields['pos']), int(fields['stream_index'])
        start, duration = Decimal(fields['pts_time']), Decimal(fields['duration_time'])
        if stream < 0 or not start.is_finite() or not duration.is_finite() or duration <= 0:
            raise ValueError('Missing or invalid packet timing')
        index = bisect_right(self.starts, position) - 1
        if index < 0 or position >= self.extents[index][1]:
            raise ValueError('Packet position outside fragmented payload')
        fragment = self.extents[index][2]
        tracks = self.fragments.setdefault(fragment, {})
        track = tracks.setdefault(stream, {'start': start, 'end': start + duration, 'packets': 0})
        track['start'] = min(track['start'], start)
        track['end'] = max(track['end'], start + duration)
        track['packets'] += 1
        self.packets += 1

    def report(self):
        previous_ends, rows = {}, []
        for fragment, tracks in sorted(self.fragments.items()):
            serialized = {}
            for stream, timing in sorted(tracks.items()):
                serialized[str(stream)] = {
                    'startSeconds': float(timing['start']), 'endSeconds': float(timing['end']),
                    'packets': timing['packets'],
                    'gapFromPreviousTrackEndMs': float((timing['start'] - previous_ends[stream]) * 1000)
                        if stream in previous_ends else None,
                }
                previous_ends[stream] = timing['end']
            # Missing tracks are explicit, not inferred as corrupt or assigned invented timestamps.
            end_spread = max(t['end'] for t in tracks.values()) - min(t['end'] for t in tracks.values())
            rows.append({'fragment': fragment, 'tracks': serialized,
                         'trackEndSpreadMs': float(end_spread * 1000) if len(tracks) > 1 else None})
        spreads = sorted(row['trackEndSpreadMs'] for row in rows if row['trackEndSpreadMs'] is not None)
        return {'packetCount': self.packets, 'fragmentCount': len(rows),
                'multiplexedFragmentCount': len(spreads),
                'maximumTrackEndSpreadMs': max(spreads) if spreads else None,
                'fragments': rows,
                'limits': 'ffprobe presentation-time envelopes, rounded to its printed precision; stream indices are not codec identities. Does not model MSE coded-frame processing, prove browser drift, or assess perceptual smoothness.'}


def inspect(path, ffprobe='ffprobe'):
    path = Path(path).resolve(strict=True)
    if not path.is_file():
        raise ValueError('Expected a local media file')
    timing = FragmentTiming(media_extents(path))
    code, _, nonempty, diagnostics = bounded_process([
        ffprobe, '-v', 'error', '-protocol_whitelist', 'file,pipe',
        '-show_packets', '-show_entries', 'packet=stream_index,pts_time,duration_time,pos',
        '-of', 'compact=p=1:nk=0', str(path)], timing.add, timeout=1800)
    if code or nonempty or not timing.packets:
        raise ValueError('Packet timing probe failed: ' + diagnostics)
    return timing.report()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.input)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(report, output, indent=2)
