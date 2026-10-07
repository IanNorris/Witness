"""Manual, explicitly confirmed move-and-Stop for approved physical cameras 10/12."""
import argparse
import contextlib
import os
from pathlib import Path
import re
import sqlite3
import subprocess
from quiet_subprocess import quiet_process_options
from urllib.parse import unquote, urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--camera', type=int, choices=(10, 12), required=True)
    parser.add_argument('--confirm-physical-movement', action='store_true')
    args = parser.parse_args()
    if not args.confirm_physical_movement:
        parser.error('explicit physical movement confirmation required')
    with contextlib.closing(sqlite3.connect(args.database.resolve().as_uri() + '?mode=ro', uri=True)) as db:
        row = db.execute('SELECT LinkedCameraId,PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword,CameraString FROM Camera WHERE CameraUID=?', (args.camera,)).fetchone()
    if not row or not row[1]:
        parser.error('PTZ must be enabled for the explicitly approved camera')
    # Links can be reciprocal companion feeds, not a physical-parent pointer.
    # Never follow them: use only this approved camera's own endpoint.
    _, _, host, port, user, password, url = row
    parsed = urlsplit(url or '')
    host = host or parsed.hostname
    user = user or unquote(parsed.username or '')
    password = password or unquote(parsed.password or '')
    if not host or not user or not password:
        parser.error('PTZ configuration incomplete')
    env = os.environ.copy()
    env.update(WITNESS_PTZ_PHYSICAL_CONFIRM=str(args.camera), WITNESS_PTZ_PROBE_HOST=host,
               WITNESS_PTZ_PROBE_USERNAME=user, WITNESS_PTZ_PROBE_PASSWORD=password,
               WITNESS_PTZ_PROBE_PORT=str(port if port and port > 0 else 443))
    # Never impose subprocess timeout mid-movement: native watchdog owns Stop.
    result = subprocess.run([str(args.probe.resolve(strict=True))], env=env,
                            capture_output=True, text=True, **quiet_process_options())
    for line in result.stdout.splitlines():
        if re.fullmatch(r'preflightHttpStop=[01](?: preflightBaichuanStop=[01](?: moveAck=[01] baichuanStopAck=[01] httpFallbackStopAck=[01] stopDispatchMs=\d+ stopAckMs=\d+)?)?', line):
            print(line, flush=True)
    print(f'camera={args.camera} probeExit={result.returncode}', flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
