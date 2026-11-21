"""Opt-in dashboards v2 and access-hardening integration against a private server.

Run with:
    set WITNESS_DASHBOARD_TEST_SERVER=build-vs2026-stable\\bin\\RelWithDebInfo\\WitnessServer.exe
    python -m unittest discover -s Tools/Tests -p test_dashboard_http.py

A fresh database is created in a temporary directory under artifacts/. The
cameras point at an unroutable loopback port, so no media is involved.
Principals: 1 = admin (groups A and B), 2 = member of A (cameras 1, 2),
3 = member of B (camera 3).
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
from test_full_server_isolation import loopback_port


def camera_tile(tile_id, camera, x, y=0, w=12, h=12):
    return {'id': tile_id, 'type': 'camera', 'camera': camera, 'x': x, 'y': y, 'w': w, 'h': h}


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('WITNESS_DASHBOARD_TEST_SERVER'),
                     'Set WITNESS_DASHBOARD_TEST_SERVER to a built WitnessServer.exe')
class DashboardHttpTests(unittest.TestCase):
    def test_dashboards_and_access(self):
        server = Path(os.environ['WITNESS_DASHBOARD_TEST_SERVER']).resolve(strict=True)
        repository = Path(__file__).resolve().parents[2]
        (repository / 'artifacts').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dashboard-http-', dir=repository / 'artifacts') as directory:
            root = Path(directory)
            data, cache = root / 'data', root / 'cache'
            cache.mkdir()
            environment = dict(os.environ, WITNESS_DATA_ROOT=str(data))
            created = subprocess.run([str(server), '/createdb'], cwd=root, env=environment,
                                     capture_output=True, timeout=30, **quiet_process_options())
            self.assertEqual(created.returncode, 0, created.stderr.decode(errors='replace'))
            port = loopback_port()
            tokens = [secrets.token_hex(32) for _ in range(3)]
            csrfs = [secrets.token_hex(32) for _ in range(3)]
            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                for index, (token, csrf) in enumerate(zip(tokens, csrfs)):
                    uid = db.execute('INSERT INTO User(Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin) VALUES(?,?,?,?,?,?)',
                        (f'fixture-{index}', f'Fixture {index}', 'not-a-login-hash', 0, 1, 1 if index == 0 else 0)).lastrowid
                    db.execute('INSERT INTO Session VALUES(?,?,?,?)', (token, csrf, uid, int(time.time())))
                settings = {'server_hostname': f'127.0.0.1:{port}', 'server_tls_mode': 'NoSecurity',
                    'server_cache': str(cache), 'clip_cleanup_enabled': '0', 'thread_count': '2'}
                db.executemany('INSERT OR REPLACE INTO Setting VALUES(?,?)', settings.items())
                for name in ('Front', 'Side', 'Private'):
                    db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,MotionFilter,BlackoutMaskPath,FocusMaskPath) VALUES(?,?,?,?,?,?,?,?,?,?)',
                        (name, 'rtsp://127.0.0.1:1/none', '', 1, 1, 400, 0.99, '', '', ''))
                db.executemany('INSERT INTO CameraGroup(DisplayName,Description) VALUES(?,?)', [('A', ''), ('B', '')])
                db.executemany('INSERT INTO CameraGroupMapping VALUES(?,?)', [(1, 1), (2, 1), (3, 2)])
                db.executemany('INSERT INTO UserGroupMapping VALUES(?,?)', [(1, 1), (1, 2), (2, 1), (3, 2)])
                now = int(time.time())
                db.executemany('INSERT INTO Clip(Timestamp,Camera,MotionTimestamp,ActiveDuration,Duration,RecordMode,MaxMotion,Description,Save,Tags,Reviewed) VALUES(?,?,?,?,?,?,?,?,?,?,0)',
                    [(now - 600, 1, now - 600, 5, 10, 0, 1, '', 0, ''), (now - 500, 3, now - 500, 5, 10, 0, 1, '', 0, '')])
                db.commit()

            def request(path, body=None, principal=1, csrf=True):
                headers = {} if principal is None else {'Cookie': f'SessionToken-{port}={tokens[principal - 1]}'}
                payload = None
                if body is not None:
                    if csrf and principal is not None:
                        body = dict(body, csrf=csrfs[principal - 1])
                    headers['Content-Type'] = 'application/json'
                    payload = json.dumps(body).encode()
                req = urllib.request.Request(f'http://127.0.0.1:{port}{path}', headers=headers, data=payload)
                try:
                    response = urllib.request.urlopen(req, timeout=5)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    raw = response.read()
                    try:
                        parsed = json.loads(raw) if raw else None
                    except ValueError:
                        parsed = raw
                    return response.status, parsed, response.headers

            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            with (root / 'server.log').open('wb') as log:
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
                    self.check_dashboards(request)
                    self.check_access_hardening(request, data)
                    self.check_long_session(request)
                finally:
                    with contextlib.suppress(Exception):
                        process.terminate()
                        process.wait(timeout=15)

    def check_dashboards(self, request):
        doc = {'schema': 2, 'tiles': [camera_tile('a', 1, 0)]}
        self.assertEqual(request('/dashboard/enum', principal=None)[0], 401)
        code, body, _ = request('/dashboard/create', {'name': 'Mine', 'document': doc}, principal=2)
        self.assertEqual(code, 200, body)
        personal = body['id']

        # Inaccessible and nonexistent cameras are rejected identically.
        code, hidden, _ = request('/dashboard/create', {'name': 'X', 'document': {'tiles': [camera_tile('a', 3, 0)]}}, principal=2)
        code2, missing, _ = request('/dashboard/create', {'name': 'X', 'document': {'tiles': [camera_tile('a', 999, 0)]}}, principal=2)
        self.assertEqual((code, code2), (422, 422))
        self.assertEqual(hidden['error'], missing['error'])
        self.assertEqual(request('/dashboard/create', {'name': 'X', 'document': doc}, principal=2, csrf=False)[0], 401)
        self.assertEqual(request('/dashboard/create', {'name': 'X', 'scope': 'shared', 'document': doc}, principal=2)[0], 422)

        # Personal dashboards are private, admins included.
        self.assertEqual(request(f'/dashboard/get/{personal}', principal=3)[0], 404)
        self.assertEqual(request(f'/dashboard/get/{personal}', principal=1)[0], 404)
        self.assertEqual(request(f'/dashboard/get/{personal}', principal=2)[0], 200)

        # Shared dashboard with a camera user 2 cannot see.
        shared_doc = {'tiles': [camera_tile('a', 1, 0), camera_tile('b', 3, 12)]}
        code, body, _ = request('/dashboard/create', {'name': 'Wall', 'scope': 'shared', 'document': shared_doc})
        self.assertEqual(code, 200, body)
        shared = body['id']
        self.assertEqual(request(f'/dashboard/get/{shared}', principal=2)[0], 404)
        self.assertEqual(request('/dashboard/share', {'id': shared, 'principals': [{'type': 'group', 'id': 1}]})[0], 200)
        code, body, _ = request(f'/dashboard/get/{shared}', principal=2)
        self.assertEqual(code, 200)
        tiles = body['document']['tiles']
        self.assertEqual(tiles[1]['type'], 'unavailable')
        self.assertNotIn('camera', tiles[1])
        self.assertEqual(body['redacted'], 1)
        self.assertFalse(body['canEdit'])
        self.assertNotIn('shares', body)
        self.assertEqual(request(f'/dashboard/get/{shared}', principal=3)[0], 404)

        # Recipients cannot edit; stale revisions conflict.
        self.assertEqual(request('/dashboard/update', {'id': shared, 'baseRevision': 1, 'name': 'X'}, principal=2)[0], 422)
        self.assertEqual(request('/dashboard/update', {'id': shared, 'baseRevision': 1, 'name': 'Wall 2'})[0], 200)
        code, body, _ = request('/dashboard/update', {'id': shared, 'baseRevision': 1, 'name': 'Wall 3'})
        self.assertEqual((code, body['revision']), (409, 2))

        # Saving a redacted view keeps the hidden reference (admin lacking access is
        # simulated by user 2 on their own dashboard after losing camera 2 is not
        # needed here: the merge is unit-tested). Everyone principal reaches user 3.
        self.assertEqual(request('/dashboard/share', {'id': shared, 'principals': [{'type': 'everyone'}]})[0], 200)
        code, body, _ = request(f'/dashboard/get/{shared}', principal=3)
        self.assertEqual(code, 200)
        self.assertEqual(body['document']['tiles'][0]['type'], 'unavailable')
        code, body, _ = request(f'/dashboard/access_preview/{shared}')
        self.assertEqual(code, 200)
        self.assertEqual({v['userId']: v['redacted'] for v in body['viewers']}, {1: 0, 2: 1, 3: 1})
        self.assertEqual(request(f'/dashboard/access_preview/{shared}', principal=2)[0], 404)

        code, body, _ = request(f'/dashboard/history/{shared}')
        self.assertEqual((code, [r['revision'] for r in body['revisions']]), (200, [2, 1]))
        self.assertEqual(request(f'/dashboard/history/{shared}', principal=2)[0], 404)

        code, body, _ = request('/dashboard/enum', principal=2)
        self.assertEqual(sorted(d['id'] for d in body['dashboards']), sorted([personal, shared]))

        self.assertEqual(request('/dashboard/delete', {'id': personal}, principal=3)[0], 404)
        self.assertEqual(request('/dashboard/delete', {'id': personal}, principal=2)[0], 200)
        self.shared = shared

    def check_access_hardening(self, request, data):
        year, month = time.localtime().tm_year, time.localtime().tm_mon
        code, body, _ = request(f'/clip/calendar/{year}/{month}', principal=2)
        self.assertEqual(code, 200)
        self.assertEqual(sum(day['count'] for day in body['days']), 1)
        code, body, _ = request(f'/clip/calendar/{year}/{month}', principal=1)
        self.assertEqual(sum(day['count'] for day in body['days']), 2)
        self.assertEqual(request('/api/detection/3?from=0&to=1', principal=2)[0], 404)
        self.assertEqual(request('/api/detection/1?from=0&to=1', principal=2)[0], 200)

        # Marking everything reviewed only touches accessible clips.
        self.assertEqual(request('/clip/review', {'all': True}, principal=2)[0], 200)
        with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
            reviewed = dict(db.execute('SELECT Camera, Reviewed FROM Clip').fetchall())
        self.assertEqual(reviewed, {1: 1, 3: 0})

        # Group delete removes its mappings and dashboard shares.
        self.assertEqual(request('/dashboard/share', {'id': self.shared, 'principals': [{'type': 'group', 'id': 2}]})[0], 200)
        self.assertEqual(request('/group/delete', {'id': 2})[0], 200)
        with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM CameraGroupMapping WHERE `Group` = 2').fetchone()[0], 0)
            self.assertEqual(db.execute('SELECT COUNT(*) FROM UserGroupMapping WHERE `Group` = 2').fetchone()[0], 0)
            self.assertEqual(db.execute('SELECT COUNT(*) FROM DashboardShare WHERE PrincipalType = 2').fetchone()[0], 0)

    def check_long_session(self, request):
        code, body, headers = request('/auth/profile', {}, principal=2, csrf=False)
        self.assertEqual(code, 200)
        self.assertFalse(body['longSession'])
        self.assertIsNone(headers.get('Set-Cookie'))
        self.assertEqual(request('/auth/toggle_long_session', {'username': 'fixture-1', 'value': True})[0], 200)
        code, body, headers = request('/auth/profile', {}, principal=2, csrf=False)
        self.assertTrue(body['longSession'])
        self.assertIn(f'max-age={60 * 60 * 24 * 400}', headers.get('Set-Cookie', ''))
        self.assertEqual(request('/auth/toggle_long_session', {'username': 'fixture-1', 'value': True}, principal=2)[0], 400)


if __name__ == '__main__':
    unittest.main()
