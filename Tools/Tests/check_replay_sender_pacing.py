"""Measure loopback replay delivery independently of Witness/browser decoding.

Uses the full-server fixture's exact sender and a packet-copy-only receiver.
This is a workload qualification tool, not a camera or production stability test.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from test_full_server_isolation import loopback_port, replay_senders


def delivery_rate(samples, warmup=10):
    if any(type(sample.get(name)) not in (int, float) or
           not math.isfinite(sample[name]) or sample[name] < 0
           for sample in samples for name in ('wallSeconds', 'mediaSeconds')):
        raise ValueError('invalid progress clock sample')
    usable = [sample for sample in samples if sample['wallSeconds'] >= warmup]
    if len(usable) < 2:
        raise ValueError('insufficient progress after warmup')
    first, last = usable[0], usable[-1]
    wall = last['wallSeconds'] - first['wallSeconds']
    media = last['mediaSeconds'] - first['mediaSeconds']
    if wall <= 0 or media <= 0:
        raise ValueError('non-advancing progress')
    steps = [(right['wallSeconds'] - left['wallSeconds'],
              right['mediaSeconds'] - left['mediaSeconds'])
             for left, right in zip(usable, usable[1:])]
    if any(wall_step <= 0 or media_step < 0 for wall_step, media_step in steps):
        raise ValueError('non-monotonic progress clock')
    gaps = sorted(wall_step for wall_step, _ in steps)
    return {'wallSpanSeconds': wall, 'mediaSpanSeconds': media, 'mediaPerWallSecond': media / wall,
            'progressCadence': {
                'scope': 'FFmpegReceiverProgressReceiptIntervalsNotExactPacketArrivalTiming',
                'samplesAfterWarmup': len(usable),
                'maximumReceiptGapSeconds': gaps[-1],
                'p95ReceiptGapSeconds': gaps[math.ceil(len(gaps) * .95) - 1],
                'maximumObservedMediaStepSeconds': max(media_step for _, media_step in steps),
                'maximumWallMinusMediaStepSeconds': max(wall_step - media_step for wall_step, media_step in steps),
            }}


def inspect(source, seconds):
    source = Path(source).resolve(strict=True)
    if not source.is_file() or source.stat().st_size > 512 * 1024 * 1024 or not 20 <= seconds <= 120:
        raise ValueError('expected bounded local input and 20..120 second media interval')
    artifacts = Path(__file__).resolve().parents[2] / 'artifacts'
    url = f'http://127.0.0.1:{loopback_port()}/replay'
    with tempfile.TemporaryDirectory(prefix='sender-pacing-', dir=artifacts) as directory:
        root = Path(directory)
        with replay_senders([source], [url], root), (root / 'receiver.log').open('wb') as errors:
            process = subprocess.Popen(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'warning',
                '-i', url, '-map', '0:v:0', '-an', '-c:v', 'copy', '-t', str(seconds),
                '-stats_period', '1', '-progress', 'pipe:1', '-f', 'null', '-'],
                stdout=subprocess.PIPE, stderr=errors, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
            # A stalled receiver must not leave a listener alive or block a turn.
            expired = threading.Event()
            def timeout():
                expired.set()
                if process.poll() is None:
                    process.kill()
            timer = threading.Timer(seconds * 2 + 20, timeout)
            timer.start()
            started = time.monotonic()
            samples, fields = [], {}
            try:
                for line in process.stdout:
                    key, _, value = line.strip().partition('=')
                    fields[key] = value
                    if key == 'progress' and 'out_time_us' in fields:
                        samples.append({'wallSeconds': time.monotonic() - started,
                                        'mediaSeconds': int(fields['out_time_us']) / 1e6})
                        fields = {}
                process.wait(timeout=10)
                if expired.is_set() or process.returncode:
                    raise ValueError('receiver failed or timed out')
            finally:
                timer.cancel()
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)
                process.stdout.close()
            errors.flush()
            return {'samples': samples, **delivery_rate(samples),
                    'receiverDiagnosticsBytes': (root / 'receiver.log').stat().st_size,
                    'limits': 'Packet-copy replay and receiver only; no Witness, browser, decode or physical network'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--seconds', type=int, default=60)
    args = parser.parse_args()
    print(json.dumps(inspect(args.source, args.seconds), indent=2))
