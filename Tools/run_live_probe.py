"""Run a bounded native camera probe using credentials from a read-only DB copy."""
import argparse
import os
from pathlib import Path
import sqlite3
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('database', type=Path)
p.add_argument('camera', type=int)
p.add_argument('seconds', type=int)
p.add_argument('output', type=Path)
p.add_argument('--main', action='store_true')
p.add_argument('--capture', action='store_true')
p.add_argument('--capture-delay', type=int, default=0)
p.add_argument('--reolink', action='store_true')
args = p.parse_args()
with sqlite3.connect(args.database.resolve().as_uri() + '?mode=ro', uri=True) as db:
    row = db.execute('SELECT CameraString, CameraStringSub FROM Camera WHERE CameraUID=?', (args.camera,)).fetchone()
if not row:
    raise SystemExit('Camera not found')
env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
env['Path'] = os.environ.get('PATH', '')
env['WITNESS_PREVIEW_PROBE_URL'] = row[0 if args.main else 1]
if not env['WITNESS_PREVIEW_PROBE_URL']:
    raise SystemExit('Selected camera has no stream URL')
if args.capture:
    env['WITNESS_PROBE_CAPTURE'] = '1'
    env['WITNESS_PROBE_CAPTURE_DELAY'] = str(args.capture_delay)
if args.reolink:
    env['WITNESS_PROBE_REOLINK'] = '1'
result = subprocess.run(['build-vs2026/bin/RelWithDebInfo/LivePreviewProbe.exe', str(args.seconds),
    str(args.output), str(args.camera)], env=env, timeout=args.seconds + 90)
raise SystemExit(result.returncode)
