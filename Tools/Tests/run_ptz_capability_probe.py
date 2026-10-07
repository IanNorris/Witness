"""Read-only PTZ discovery using the production transport; never moves a motor."""
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
    parser.add_argument('--camera', type=int, required=True)
    args = parser.parse_args()
    if args.camera <= 0: parser.error('camera must be positive')
    with contextlib.closing(sqlite3.connect(args.database.resolve().as_uri()+'?mode=ro', uri=True)) as db:
        linked = db.execute('SELECT LinkedCameraId FROM Camera WHERE CameraUID=? AND LinkedCameraId>0', (args.camera,)).fetchone()
        # Use the configured physical parent, matching the production PTZ lane.
        device = linked[0] if linked and linked[0] else args.camera
        row = db.execute('SELECT PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword,CameraString FROM Camera WHERE CameraUID=?', (device,)).fetchone()
    if not row or not row[0]: parser.error('PTZ is not enabled')
    _,host,port,user,password,url = row
    parsed = urlsplit(url or '')
    host = host or parsed.hostname
    user = user or unquote(parsed.username or '')
    password = password or unquote(parsed.password or '')
    if not host or not user or not password: parser.error('PTZ configuration is incomplete')
    env = os.environ.copy()
    env.update(WITNESS_PTZ_PROBE_HOST=host,WITNESS_PTZ_PROBE_USERNAME=user,
               WITNESS_PTZ_PROBE_PASSWORD=password,WITNESS_PTZ_PROBE_PORT=str(port if port and port > 0 else 443))
    result = subprocess.run([str(args.probe.resolve())],env=env,capture_output=True,text=True,timeout=100,
                            **quiet_process_options())
    # Never relay transport logs, URLs, credentials or arbitrary exception text.
    for line in result.stdout.splitlines():
        if re.fullmatch(r'authenticated=[01](?: positionValid=[01] zoomValid=[01] zoomMin=-?\d+ zoomMax=-?\d+ zoomPosition=-?\d+ presetCount=\d+ enabledPresets=\d+ elapsedMs=\d+)?',line): print(line)
    print(f'probeExit={result.returncode}',flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
