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
import datetime
import unittest
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
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
        onvif_events = []
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
                            if self.path.startswith('/onvif/'):
                                soap='http://www.w3.org/2003/05/soap-envelope'; schema='http://www.onvif.org/ver10/schema'
                                operation=ET.fromstring(self.rfile.read(int(self.headers['Content-Length']))).find('{'+soap+'}Body')[0]
                                namespace,name=operation.tag[1:].split('}')
                                onvif_events.append((camera,name))
                                content=''
                                if name=='GetSystemDateAndTime':
                                    now=datetime.datetime.now(datetime.timezone.utc)
                                    content=f'<m:SystemDateAndTime><t:UTCDateTime><t:Date><t:Year>{now.year}</t:Year><t:Month>{now.month}</t:Month><t:Day>{now.day}</t:Day></t:Date><t:Time><t:Hour>{now.hour}</t:Hour><t:Minute>{now.minute}</t:Minute><t:Second>{now.second}</t:Second></t:Time></t:UTCDateTime></m:SystemDateAndTime>'
                                elif name=='GetCapabilities':
                                    content='<m:Capabilities>' + ('<t:Media><t:XAddr>/onvif/media</t:XAddr></t:Media><t:PTZ><t:XAddr>/onvif/ptz</t:XAddr></t:PTZ>' if camera==1 else '') + '</m:Capabilities>'
                                elif name=='GetProfiles': content='<m:Profiles token="profile"><t:PTZConfiguration token="config"/></m:Profiles>'
                                elif name=='GetConfigurationOptions': content='<m:PTZConfigurationOptions><t:Spaces><t:ContinuousPanTiltVelocitySpace><t:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</t:URI><t:XRange><t:Min>-1</t:Min><t:Max>1</t:Max></t:XRange><t:YRange><t:Min>-1</t:Min><t:Max>1</t:Max></t:YRange></t:ContinuousPanTiltVelocitySpace></t:Spaces><t:PTZTimeout><t:Min>PT1S</t:Min><t:Max>PT10S</t:Max></t:PTZTimeout></m:PTZConfigurationOptions>'
                                elif name=='GetPresets': content='<m:Preset token="gate"><t:Name>Gate</t:Name></m:Preset>'
                                payload=f'<s:Envelope xmlns:s="{soap}" xmlns:m="{namespace}" xmlns:t="{schema}"><s:Body><m:{name}Response>{content}</m:{name}Response></s:Body></s:Envelope>'.encode()
                                self.send_response(200);self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload);return
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
                    db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                        ('ONVIF fixture', 'rtsp://fixture:fixture@127.0.0.1/stream1', '', 0, 1, 400, .99, 1, '127.0.0.1', ports[1], 'fixture', 'fixture'))
                    db.execute('INSERT INTO CameraGroupMapping VALUES(5,1)');db.commit()
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
                        status, capability = request('/ptz/4/zoom')
                        self.assertEqual(status, 200); self.assertFalse(json.loads(capability)['panTiltSupported'])
                        self.assertEqual(request('/ptz/4/presets')[0], 502)
                        self.assertEqual(request('/ptz/4/left', {'csrf': csrf, 'speed': 1})[0], 502)
                        unsupported_ms = (time.monotonic() - start) * 1000
                        self.assertLess(unsupported_ms, 500); self.assertEqual(events, [])
                        status, capability=request('/ptz/5/zoom')
                        self.assertEqual(status,200);self.assertTrue(json.loads(capability)['panTiltSupported']);self.assertFalse(json.loads(capability)['zoomSupported'])
                        self.assertEqual(request('/ptz/5/presets')[0],200)
                        self.assertEqual(request('/ptz/5/left',{'csrf':csrf,'speed':32})[0],200)
                        self.assertEqual(request('/ptz/5/stop',{'csrf':csrf,'speed':32})[0],200)
                        self.assertEqual(request('/ptz/5/preset',{'csrf':csrf,'id':1})[0],200)
                        self.assertEqual([name for camera,name in onvif_events if camera==1][-3:],['ContinuousMove','Stop','GotoPreset'])
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
                        def wait_active(path):
                            deadline=time.monotonic()+3
                            while time.monotonic()<deadline:
                                status,body=request('/debug/health?workSamples=0&details=1',timeout=1)
                                self.assertEqual(status,200)
                                active=json.loads(body)['server']['http']['activeRequests']
                                if any(row['path']==path and row['elapsedMs']>=50 for row in active): return
                                time.sleep(.01)
                            self.fail(f'Request never reached server: {path}')
                        with ThreadPoolExecutor(max_workers=3) as pool:
                            poll = pool.submit(request, '/ptz/1/zoom', timeout=10)
                            self.assertTrue(entered[0].wait(2))
                            gesture = pool.submit(request, '/ptz/1/left', {'csrf': csrf, 'speed': 1}, timeout=10)
                            wait_active('/ptz/1/left')
                            stop = pool.submit(request, '/ptz/1/stop', {'csrf': csrf}, timeout=10)
                            wait_active('/ptz/1/stop'); release.set()
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
