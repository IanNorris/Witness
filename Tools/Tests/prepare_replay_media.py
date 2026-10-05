"""Create a payload-preserving constant-cadence workload copy, never alter source.

For decoder/load comparisons only: this intentionally replaces captured timing
and removes audio, so it is not evidence of original camera cadence or A/V sync.
"""
import argparse
from fractions import Fraction
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from check_preview_decode import inspect


def probe(path, entries, packets=False):
    command = ['ffprobe', '-v', 'error', '-protocol_whitelist', 'file,pipe', '-select_streams', 'v:0']
    if packets:
        command += ['-show_packets', '-show_data_hash', 'sha256']
    command += ['-show_entries', entries, '-of', 'json', str(path)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode or result.stderr.strip():
        raise ValueError('media probe diagnostics')
    return json.loads(result.stdout)


def validate_stream(stream, fps):
    if stream.get('codec_name') not in ('h264', 'hevc') or stream.get('has_b_frames') != 0:
        raise ValueError('only explicit no-B-frame H.264/HEVC media can be normalized')
    if not 1 <= fps <= 60:
        raise ValueError('fps must be 1..60')
    if not (0 < stream.get('width', 0) <= 7680 and 0 < stream.get('height', 0) <= 4320):
        raise ValueError('invalid dimensions')


def payload_hashes(path):
    packets = probe(path, 'packet=data_hash', packets=True)['packets']
    values = [packet.get('data_hash') for packet in packets]
    if len(values) < 2 or any(not isinstance(value, str) or not value.startswith('SHA256:') for value in values):
        raise ValueError('missing packet payload hashes')
    return values


def prepare(source, output, fps):
    source, output = Path(source).resolve(strict=True), Path(output).absolute()
    if not source.is_file() or source.stat().st_size > 512 * 1024 * 1024:
        raise ValueError('expected local media no larger than 512 MiB')
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ValueError('output must be new with an existing parent')
    if output.parent.is_symlink() or output.parent.is_junction():
        raise ValueError('linked output parent rejected')
    stream = probe(source, 'stream=codec_name,width,height,has_b_frames')['streams'][0]
    validate_stream(stream, fps)
    original_hashes = payload_hashes(source)
    with tempfile.TemporaryDirectory(prefix='replay-media-', dir=output.parent) as directory:
        staged = Path(directory) / 'workload.mp4'
        result = subprocess.run(['ffmpeg', '-nostdin', '-n', '-v', 'error',
            '-protocol_whitelist', 'file,pipe', '-i', str(source), '-map', '0:v:0', '-an', '-c:v', 'copy',
            '-bsf:v', f'setts=pts=N/({fps}*TB):dts=N/({fps}*TB):duration=1/({fps}*TB)',
            '-video_track_timescale', '90000', str(staged)], capture_output=True, text=True, timeout=120)
        if result.returncode or result.stderr.strip():
            raise ValueError('workload remux diagnostics')
        checked = inspect(staged)
        if not checked['passed'] or payload_hashes(staged) != original_hashes:
            raise ValueError('strict decode or exact packet payload preservation failed')
        timing = probe(staged, 'stream=avg_frame_rate,r_frame_rate,nb_frames')['streams'][0]
        if Fraction(timing['avg_frame_rate']) != fps or Fraction(timing['r_frame_rate']) != fps:
            raise ValueError('normalized cadence metadata is not exact')
        # Same-volume publication cannot replace an existing output, even if
        # another process creates it after the initial existence check.
        os.link(staged, output)
    return {'passed': True, 'codec': stream['codec_name'], 'width': stream['width'], 'height': stream['height'],
            'fps': fps, 'packets': len(original_hashes), 'decodedFrames': checked['frames']['samples'],
            'packetPayloadsIdentical': True, 'strictDecodePassed': True,
            'limits': 'Timing normalized and audio removed; decoder workload only, not original pacing or A/V sync'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--fps', type=int, required=True)
    args = parser.parse_args()
    try:
        report = prepare(args.source, args.output, args.fps)
    except (ValueError, OSError, KeyError, IndexError, subprocess.TimeoutExpired):
        print(json.dumps({'passed': False, 'preparationFailed': True}))
        sys.exit(2)
    print(json.dumps(report, sort_keys=True))
