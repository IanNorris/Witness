"""Actual Crow/HTTP PTZ reconfiguration against loopback motor mocks only."""
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from contextlib import ExitStack, closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import sqlite3
import threading


def check_http_relink(request, csrf, inputs, database):
    if len(inputs) < 3:
        raise ValueError('HTTP relink fixture needs three disposable cameras')
    events, events_lock = [], threading.Lock()
    entered, release = threading.Event(), threading.Event()
    state = {'fail_stop': False, 'hold_left': False}

    def start_camera(label, stack):
        class Mock(BaseHTTPRequestHandler):
            def setup(self):
                super().setup()
                self.connection.settimeout(0.5)  # Close initial invalid TLS promptly.

            def log_message(self, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 4096:
                    self.send_error(400)
                    return
                command = json.loads(self.rfile.read(length))[0]
                name = command.get('cmd')
                code, value = 0, {}
                if name == 'Login':
                    value = {'Token': {'name': 'fixture-only', 'leaseTime': 3600}}
                elif name == 'PtzCtrl':
                    operation = command.get('param', {}).get('op')
                    if operation not in {'Left', 'Right', 'Stop'}:
                        self.send_error(403)
                        return
                    with events_lock:
                        events.append(f'{label}.{operation}')
                    if label == 'A' and operation == 'Left' and state['hold_left']:
                        entered.set()
                        release.wait(6)
                    if label == 'B' and operation == 'Stop' and state['fail_stop']:
                        code = 1
                else:
                    self.send_error(403)
                    return
                body = json.dumps([{'cmd': name, 'code': code, 'value': value}]).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Mock)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        stack.callback(server.server_close)
        stack.callback(worker.join, 2)
        stack.callback(server.shutdown)
        return server.server_port

    def update(body, expected=200):
        status, _ = request('/camera/admin_update', body=body, timeout=8)
        if status != expected:
            raise AssertionError(f'Camera update returned {status}, expected {expected}')

    def linked():
        with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as db:
            return db.execute('SELECT LinkedCameraId FROM Camera WHERE CameraUID=2').fetchone()[0]

    def move(camera, direction):
        return request(f'/ptz/{camera}/{direction}', body={'csrf': csrf, 'speed': 2}, timeout=8)

    with ExitStack() as stack:
        port_a, port_b = start_camera('A', stack), start_camera('B', stack)
        base = {'csrf': csrf, 'enabled': 1, 'ptzEnabled': 1,
                'ptzUsername': 'fixture-only', 'ptzPassword': 'fixture-only'}
        a = {**base, 'id': 1, 'displayName': 'Mock A', 'connectionString': str(inputs[0]),
             'ptzApiHost': '127.0.0.1', 'ptzApiPort': port_a}
        b = {**base, 'id': 3, 'displayName': 'Mock B', 'connectionString': str(inputs[2]),
             'ptzApiHost': '127.0.0.1', 'ptzApiPort': port_b}
        view = {'csrf': csrf, 'id': 2, 'displayName': 'Linked mock view', 'enabled': 1,
                'connectionString': str(inputs[1]), 'ptzEnabled': 1, 'linkedCameraId': 1}
        try:
            update(a); update(b); update(view)
            body = {'csrf': csrf, 'speed': 2}
            if request('/ptz/2/left', False, body)[0] != 403:
                raise AssertionError('Anonymous movement was not rejected')
            if request('/ptz/2/left', body={**body, 'csrf': 'invalid'})[0] != 403 or events:
                raise AssertionError('Unauthorized movement reached mock transport')
            if move(2, 'left')[0] != 200 or move(3, 'right')[0] != 200:
                raise AssertionError('Mock movement failed')
            update({**view, 'linkedCameraId': 3})
            if linked() != 3 or events[:4] != ['A.Left', 'B.Right', 'A.Stop', 'B.Stop']:
                raise AssertionError('Relink did not stop both old components before mutation')
            if move(2, 'left')[0] != 200:
                raise AssertionError('Relinked view did not move through new transport')
            state['fail_stop'] = True
            update(view, 409)
            if linked() != 3:
                raise AssertionError('Failed Stop allowed a link mutation')
            state['fail_stop'] = False
            update(view)
            state['hold_left'] = True
            with ThreadPoolExecutor(max_workers=2) as pool:
                command = pool.submit(move, 2, 'left')
                try:
                    if not entered.wait(4):
                        raise AssertionError('Stalled mock command did not enter transport')
                    edit = pool.submit(update, {**view, 'linkedCameraId': 3})
                    try:
                        edit.result(timeout=0.2)
                        raise AssertionError('Relink crossed an active HTTP motor command')
                    except TimeoutError:
                        pass
                    if linked() != 1:
                        raise AssertionError('Configuration changed before command completed')
                finally:
                    release.set()
                if command.result(timeout=8)[0] != 200:
                    raise AssertionError('Held command did not complete')
                edit.result(timeout=8)
            if linked() != 3 or events[-2:] != ['A.Left', 'A.Stop']:
                raise AssertionError('Concurrent relink did not end original transport')
            return {'passed': True, 'events': list(events), 'physicalMotorCommands': 0,
                    'scope': 'Actual authenticated Crow routes and HTTP clients; loopback motor mocks only',
                    'failedStopPreservedLink': True, 'concurrentCommandFenced': True}
        finally:
            release.set()
            state['fail_stop'] = False
            # End retained mock leases before closing their endpoints.
            for camera in (1, 2, 3):
                try:
                    move(camera, 'stop')
                except (OSError, TimeoutError):
                    pass
