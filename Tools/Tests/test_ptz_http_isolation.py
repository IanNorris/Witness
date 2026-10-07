"""Opt-in real Crow routes with disposable DB and loopback camera mocks only."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import base64
import os
from pathlib import Path
import secrets
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from quiet_subprocess import quiet_process_options
from test_full_server_isolation import loopback_port, stop_console_process


@unittest.skipUnless(os.name == 'nt' and os.environ.get('WITNESS_PTZ_HTTP_SERVER'), 'Windows fixture not configured')
class PtzHttpIsolationTests(unittest.TestCase):
    def test_real_async_routes_and_shutdown(self):
        executable = Path(os.environ['WITNESS_PTZ_HTTP_SERVER']).resolve()
        repository = Path(__file__).resolve().parents[2]
        artifacts = repository / 'artifacts'
        release = threading.Event()
        entered = [threading.Event() for _ in range(3)]
        events = []
        events_lock = threading.Lock()
        with ExitStack() as stack:
            ports = []
            for index in range(3):
                def make_handler(camera):
                    class Mock(BaseHTTPRequestHandler):
                        def setup(self):
                            super().setup(); self.connection.settimeout(.5)
                        def log_message(self, *args): pass
                        def do_POST(self):
                            command = json.loads(self.rfile.read(int(self.headers['Content-Length'])))[0]
                            name = command['cmd']
                            with events_lock: events.append((camera, name, command.get('param', {}).get('op')))
                            value = {}
                            if name == 'Login': value = {'Token': {'name': 'fixture-only'}}
                            elif name == 'GetZoomFocus':
                                entered[camera].set(); release.wait(10)
                                value = {'ZoomFocus': {'zoom': {'pos': 100}, 'focus': {'pos': 20}}}
                            elif name != 'PtzCtrl': self.send_error(400); return
                            payload = json.dumps([{'cmd': name, 'code': 0, 'value': value}]).encode()
                            try:
                                self.send_response(200); self.send_header('Content-Length', str(len(payload)))
                                self.end_headers(); self.wfile.write(payload)
                            except (BrokenPipeError, ConnectionResetError): pass
                    return Mock
                mock = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(index))
                worker = threading.Thread(target=mock.serve_forever, daemon=True); worker.start()
                stack.callback(mock.server_close); stack.callback(worker.join, 2); stack.callback(mock.shutdown)
                ports.append(mock.server_port)
            with tempfile.TemporaryDirectory(prefix='ptz-http-isolation-', dir=artifacts) as directory:
                root = Path(directory).resolve()
                data, cache = root / 'data', root / 'cache'; cache.mkdir()
                environment = dict(os.environ, WITNESS_DATA_ROOT=str(data), WITNESS_HTTP_TRACE='1')
                subprocess.run([str(executable), '/createdb'], cwd=root, env=environment,
                    capture_output=True, timeout=30, check=True, **quiet_process_options())
                port = loopback_port(); token, csrf = secrets.token_hex(32), secrets.token_hex(32)
                with closing(sqlite3.connect(data / 'server.db')) as db:
                    db.execute('INSERT INTO User(Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin) VALUES(?,?,?,?,?,?)',
                        ('fixture-admin', 'Fixture admin', 'not-a-login-hash', 0, 1, 1))
                    db.execute('INSERT INTO Session VALUES(?,?,?,?)', (token, csrf, 1, int(time.time())))
                    db.executemany('INSERT OR REPLACE INTO Setting VALUES(?,?)',
                        {'server_hostname': f'127.0.0.1:{port}', 'server_tls_mode': 'NoSecurity',
                         'server_cache': str(cache), 'clip_cleanup_enabled': '0', 'thread_count': '2'}.items())
                    db.execute('INSERT INTO CameraGroup(DisplayName,Description) VALUES(?,?)', ('Fixture', 'Loopback only'))
                    db.execute('INSERT INTO UserGroupMapping VALUES(1,1)')
                    for camera, mock_port in enumerate(ports, 1):
                        db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                            (f'Fixture {camera}', 'fixture-no-media', '', 0, 1, 400, .99, 1, '127.0.0.1', mock_port, 'fixture', 'fixture'))
                        db.execute('INSERT INTO CameraGroupMapping VALUES(?,1)', (camera,))
                    db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                        ('Unsupported fixture', 'rtsp://fixture:fixture@127.0.0.1/stream1', '', 0, 1, 400, .99, 1, '127.0.0.1', ports[0], 'fixture', 'fixture'))
                    db.execute('INSERT INTO CameraGroupMapping VALUES(4,1)'); db.commit()
                def request(path, body=None, authenticated=True, timeout=3):
                    headers = {'Cookie': f'SessionToken-{port}={token}'} if authenticated else {}
                    if body is not None: headers['Content-Type'] = 'application/json'
                    req = urllib.request.Request(f'http://127.0.0.1:{port}' + path,
                        data=None if body is None else json.dumps(body).encode(), headers=headers)
                    try: response = urllib.request.urlopen(req, timeout=timeout)
                    except urllib.error.HTTPError as error: response = error
                    with response: return response.status, response.read()
                startup = subprocess.STARTUPINFO(); startup.dwFlags = subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
                with (root / 'server.log').open('wb') as log:
                    process = subprocess.Popen([str(executable), '--plain-console'], cwd=root, env=environment,
                        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, startupinfo=startup,
                        creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.CREATE_NEW_PROCESS_GROUP)
                    try:
                        for _ in range(100):
                            try:
                                if request('/auth/profile', {'csrf': csrf})[0] == 200: break
                            except (OSError, urllib.error.URLError): time.sleep(.1)
                        else: self.fail('fixture server not ready')
                        websocket = socket.create_connection(('127.0.0.1', port), timeout=1)
                        stream = websocket.makefile('rb')
                        key = base64.b64encode(secrets.token_bytes(16)).decode()
                        websocket.sendall((f'GET /ws/events HTTP/1.1\r\nHost: localhost\r\nCookie: SessionToken-{port}={token}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n').encode())
                        self.assertIn(b'101', stream.readline())
                        while stream.readline() != b'\r\n': pass
                        def ping():
                            before = time.monotonic()
                            websocket.sendall(b'\x89\x88\0\0\0\0ptz-test')
                            for _ in range(20):
                                header = stream.read(2)
                                self.assertEqual(len(header), 2)
                                size = header[1] & 127
                                if size == 126: size = int.from_bytes(stream.read(2), 'big')
                                elif size == 127: size = int.from_bytes(stream.read(8), 'big')
                                self.assertLess(size, 65536)
                                self.assertFalse(header[1] & 128, 'server frame unexpectedly masked')
                                payload = stream.read(size)
                                if header[0] & 15 == 10:
                                    self.assertEqual(payload, b'ptz-test')
                                    return (time.monotonic() - before) * 1000
                            self.fail('WebSocket Pong missing')
                        ping()
                        stack.callback(websocket.close); stack.callback(stream.close)
                        self.assertEqual(request('/ptz/4/zoom', authenticated=False)[0], 403)
                        self.assertEqual(request('/ptz/1/left', {'csrf': 'invalid', 'speed': 1})[0], 403)
                        start = time.monotonic()
                        self.assertEqual(request('/ptz/4/zoom')[0], 400)
                        self.assertEqual(request('/ptz/4/presets')[0], 400)
                        self.assertEqual(request('/ptz/4/left', {'csrf': csrf, 'speed': 1})[0], 400)
                        unsupported_ms = (time.monotonic() - start) * 1000
                        self.assertLess(unsupported_ms, 500); self.assertEqual(events, [])
                        with ThreadPoolExecutor(max_workers=5) as pool:
                            polls = [pool.submit(request, f'/ptz/{camera}/zoom', timeout=10) for camera in range(1, 4)]
                            for flag in entered: self.assertTrue(flag.wait(4), 'mock zoom not entered')
                            move = pool.submit(request, '/ptz/1/left', {'csrf': csrf, 'speed': 1}, timeout=10)
                            time.sleep(.1)
                            self.assertEqual(request('/ptz/1/stop', {'csrf': 'invalid'}, authenticated=False)[0], 403)
                            self.assertFalse(move.done(), 'unauthorized Stop cancelled movement')
                            stop = pool.submit(request, '/ptz/1/stop', {'csrf': csrf}, timeout=10)
                            health_ms, websocket_ms = [], []
                            for _ in range(5):
                                before = time.monotonic()
                                status, body = request('/debug/health?workSamples=0', timeout=1)
                                self.assertEqual(status, 200); health_ms.append((time.monotonic() - before) * 1000)
                                websocket_ms.append(ping())
                            time.sleep(.3); release.set()
                            for poll in polls: self.assertEqual(poll.result(timeout=5)[0], 200)
                            self.assertEqual(move.result(timeout=5)[0], 409)
                            self.assertEqual(stop.result(timeout=5)[0], 200)
                        self.assertFalse(any(op == 'Left' for _, _, op in events))
                        self.assertTrue(all(duration < 250 for duration in websocket_ms))
                        stream.close(); websocket.close()
                        # Unlike the three-worker saturation above, this gives
                        # the gesture a free worker: it waits inside the real
                        # HTTP-client lock, and Stop must invalidate it there.
                        entered[0].clear(); release.clear()
                        with ThreadPoolExecutor(max_workers=3) as pool:
                            poll = pool.submit(request, '/ptz/1/zoom', timeout=10)
                            self.assertTrue(entered[0].wait(2))
                            gesture = pool.submit(request, '/ptz/1/left', {'csrf': csrf, 'speed': 1}, timeout=10)
                            time.sleep(.1)
                            stop = pool.submit(request, '/ptz/1/stop', {'csrf': csrf}, timeout=10)
                            time.sleep(.1); release.set()
                            self.assertEqual(poll.result(timeout=5)[0], 200)
                            self.assertEqual(gesture.result(timeout=5)[0], 409)
                            self.assertEqual(stop.result(timeout=5)[0], 200)
                        self.assertFalse(any(op == 'Left' for _, _, op in events), 'cancelled gesture reached motor after HTTP read')
                        _, body = request('/debug/health?workSamples=0&details=1')
                        health = json.loads(body)
                        ptz = [row for row in health['server']['http']['recentSlowRequests'] if row['path'].endswith('/zoom')]
                        self.assertGreaterEqual(len(ptz), 3)
                        self.assertTrue(all(row['asyncCompletion'] and row['eventLoopMs'] < 100 for row in ptz), ptz)
                        # Peer disconnect while a response is deferred, followed
                        # by shutdown with another deferred request outstanding.
                        entered[0].clear(); release.clear()
                        peer = socket.create_connection(('127.0.0.1', port))
                        peer.sendall((f'GET /ptz/1/zoom HTTP/1.1\r\nHost: localhost\r\nCookie: SessionToken-{port}={token}\r\nConnection: close\r\n\r\n').encode())
                        self.assertTrue(entered[0].wait(2)); peer.close(); release.set(); time.sleep(.2)
                        self.assertEqual(request('/debug/health?workSamples=0')[0], 200)
                        entered[0].clear(); release.clear()
                        peer = socket.create_connection(('127.0.0.1', port))
                        peer.sendall((f'GET /ptz/1/zoom HTTP/1.1\r\nHost: localhost\r\nCookie: SessionToken-{port}={token}\r\n\r\n').encode())
                        self.assertTrue(entered[0].wait(2))
                        timer = threading.Timer(.5, release.set); timer.start()
                        stop_console_process(process); peer.close(); timer.join()
                        self.assertEqual(process.returncode, 0)
                        report = {'passed': True, 'unsupportedThreeRequestsMs': unsupported_ms,
                                  'nativeBuildId': health['server']['nativeBuildHash'],
                                  'webBuildId': health['server']['webBuildHash'],
                                  'healthRequestMsDuringCameraWait': health_ms,
                                  'webSocketPingMsDuringCameraWait': websocket_ms,
                                  'slowPtzRequests': ptz, 'shutdownExitCode': process.returncode,
                                  'scope': 'Disposable authenticated Crow server and loopback mocks only'}
                        output = os.environ.get('WITNESS_PTZ_HTTP_REPORT')
                        if output:
                            with Path(output).open('x', encoding='utf8') as file: json.dump(report, file, indent=2)
                        print(json.dumps(report, indent=2))
                    finally:
                        release.set()
                        if process.poll() is None: stop_console_process(process)


if __name__ == '__main__': unittest.main()
