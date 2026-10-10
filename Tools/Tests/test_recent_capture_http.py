"""Opt-in authenticated capture integration with a private database and synthetic feed.

No live configuration is copied and authentication stays enabled. The server and
paced sender are fixture-owned; only their processes are stopped on exit.
"""
import contextlib
import json
import os
from pathlib import Path
import secrets
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

from quiet_subprocess import quiet_process_options
from test_full_server_isolation import loopback_port, replay_senders, stop_console_process


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('WITNESS_CAPTURE_TEST_SERVER'),
                     'Set WITNESS_CAPTURE_TEST_SERVER and WITNESS_CAPTURE_TEST_MEDIA')
class RecentCaptureHttpTests(unittest.TestCase):
    def test_independent_capture_auth_and_media(self):
        server = Path(os.environ['WITNESS_CAPTURE_TEST_SERVER']).resolve(strict=True)
        media = Path(os.environ['WITNESS_CAPTURE_TEST_MEDIA']).resolve(strict=True)
        repository = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix='recent-capture-http-', dir=repository / 'artifacts') as directory:
            root = Path(directory)
            data, cache = root / 'data', root / 'cache'
            cache.mkdir()
            environment = dict(os.environ, WITNESS_DATA_ROOT=str(data))
            created = subprocess.run([str(server), '/createdb'], cwd=root, env=environment,
                                     capture_output=True, timeout=30, **quiet_process_options())
            self.assertEqual(created.returncode, 0, created.stderr.decode(errors='replace'))
            port = loopback_port()
            source = f'http://127.0.0.1:{loopback_port()}/replay'
            tokens, csrf = [secrets.token_hex(32) for _ in range(2)], secrets.token_hex(32)
            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                for index, token in enumerate(tokens):
                    uid = db.execute('INSERT INTO User(Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin) VALUES(?,?,?,?,?,?)',
                        (f'fixture-{index}', 'Fixture only', 'not-a-login-hash', 0, 1, 1)).lastrowid
                    db.execute('INSERT INTO Session VALUES(?,?,?,?)', (token, csrf, uid, int(time.time())))
                settings = {'server_hostname': f'127.0.0.1:{port}', 'server_tls_mode': 'NoSecurity',
                    'server_cache': str(cache), 'clip_cleanup_enabled': '0', 'thread_count': '2',
                    'continuous_recording_segment_seconds': '30'}
                db.executemany('INSERT OR REPLACE INTO Setting VALUES(?,?)', settings.items())
                db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,MotionFilter,BlackoutMaskPath,FocusMaskPath,ContinuousRecording) VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                    ('Synthetic capture', source, '', 1, 1, 400, 0.99, 'motion_vectors', '', '', 0))
                db.execute('INSERT INTO CameraGroup(DisplayName,Description) VALUES(?,?)', ('Fixture', 'Private test'))
                db.execute('INSERT INTO CameraGroupMapping VALUES(?,?)', (1, 1))
                db.executemany('INSERT INTO UserGroupMapping VALUES(?,?)', [(1, 1), (2, 1)])
                db.commit()

            def request(path, body=None, principal=0):
                headers = {} if principal is None else {'Cookie': f'SessionToken-{port}={tokens[principal]}'}
                if body is not None:
                    headers['Content-Type'] = 'application/json'
                req = urllib.request.Request(f'http://127.0.0.1:{port}{path}', headers=headers,
                    data=None if body is None else json.dumps(body).encode())
                try:
                    response = urllib.request.urlopen(req, timeout=5)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    return response.status, response.read()

            def wait_status(path):
                deadline = time.monotonic() + 40
                while time.monotonic() < deadline:
                    code, body = request(path)
                    self.assertEqual(code, 200)
                    job = json.loads(body)
                    if job['state'] in ('completed', 'failed', 'cancelled'):
                        self.assertEqual(job['state'], 'completed', job)
                        return job
                    time.sleep(0.25)
                self.fail('Capture did not finish within the bounded test window')

            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            with replay_senders([media], [source], root), (root / 'server.log').open('wb') as log:
                process = subprocess.Popen([str(server), '--plain-console'], cwd=root, env=environment,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=log, startupinfo=startup,
                    creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.CREATE_NEW_PROCESS_GROUP)
                try:
                    deadline = time.monotonic() + 40
                    while True:
                        self.assertIsNone(process.poll(), 'Private server exited during startup')
                        try:
                            if request('/debug/health')[0] == 200:
                                break
                        except (OSError, urllib.error.URLError):
                            pass
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.1)
                    time.sleep(6)  # Accumulate a preceding keyframe in the owned live input.
                    create = '/dvr/capture/1'
                    self.assertEqual(request(create, {'csrf': csrf, 'lookbackSeconds': 3}, None)[0], 403)
                    self.assertEqual(request(create, {'csrf': 'invalid', 'lookbackSeconds': 3})[0], 403)
                    code, body = request(create, {'csrf': csrf, 'lookbackSeconds': 3, 'continueRecording': True})
                    self.assertEqual(code, 202, body)
                    first = json.loads(body)
                    path = create + '/' + first['id']
                    self.assertEqual(first['state'], 'recording')
                    self.assertEqual(request(path + '/status', principal=1)[0], 404)
                    self.assertEqual(request(path + '/download', principal=None)[0], 403)
                    time.sleep(3)
                    self.assertEqual(request(path + '/stop', {'csrf': csrf})[0], 200)
                    job = wait_status(path + '/status')
                    self.assertGreater(job['actualTo'], job['actualFrom'])
                    code, output = request(path + '/download')
                    self.assertEqual(code, 200)
                    saved = root / 'capture.mp4'
                    saved.write_bytes(output)  # Generated synthetic test output only.
                    decoded = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(saved), '-f', 'null', '-'],
                        capture_output=True, timeout=30, **quiet_process_options())
                    self.assertEqual(decoded.returncode, 0, decoded.stderr.decode(errors='replace'))
                    with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                        rows = db.execute('SELECT ClipUID,RecordMode,Save,RecordingFinalized FROM Clip WHERE RecordMode=2').fetchall()
                        self.assertEqual(len(rows), 1)
                        self.assertEqual(rows[0][1:], (2, 1, 1))
                        self.assertEqual(db.execute('SELECT ContinuousRecording FROM Camera').fetchone()[0], 0)
                    captured = f'/clip/captured/{rows[0][0]}/video'
                    self.assertEqual(request(captured, principal=None)[0], 403)
                    self.assertEqual(request(captured)[0], 200)
                    code, body = request(create, {'csrf': csrf, 'lookbackSeconds': 2})
                    self.assertEqual(code, 202, body)
                    second = json.loads(body)
                    wait_status(create + '/' + second['id'] + '/status')
                finally:
                    stop_console_process(process)
                    if process.returncode != 0:
                        raise AssertionError(f'Private server exit {process.returncode}; log: {root / "server.log"}')


if __name__ == '__main__':
    unittest.main()
