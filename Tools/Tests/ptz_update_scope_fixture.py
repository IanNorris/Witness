"""Disposable loopback-only slow discovery fixture; never sends a motor command."""
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading


def check_update_scope(request, csrf, inputs, observation_seconds=1, verify_stale_discovery=False):
    if len(inputs) < 2:
        raise ValueError('PTZ update scope fixture needs two disposable cameras')
    entered = threading.Event()
    release = threading.Event()
    commands = []

    class LoginOnly(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            # AutoDetect tries TLS first. Close an invalid protocol promptly so
            # this loopback HTTP-only endpoint cannot stall the TLS handshake.
            self.connection.settimeout(0.5)

        def log_message(self, *args):
            pass

        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 4096:
                self.send_error(400)
                return
            payload = json.loads(self.rfile.read(length))
            command = payload[0].get('cmd')
            commands.append(command)
            if command != 'Login':
                self.send_error(403)
                return
            entered.set()
            release.wait(6)  # Bounded even if an assertion fails in the driver.
            body = json.dumps([{'cmd': 'Login', 'code': 0,
                               'value': {'Token': {'name': 'fixture-only', 'leaseTime': 3600}}}]).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), LoginOnly)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    first = {'csrf': csrf, 'id': 1, 'displayName': 'Fixture 1', 'connectionString': str(inputs[0]),
             'enabled': 1, 'ptzEnabled': 1, 'ptzApiHost': '127.0.0.1',
             'ptzApiPort': server.server_port, 'ptzUsername': 'fixture-only', 'ptzPassword': 'fixture-only'}
    second = {'csrf': csrf, 'id': 2, 'displayName': 'Fixture 2', 'connectionString': str(inputs[1]),
              'enabled': 1, 'ptzEnabled': 0}
    try:
        if request('/camera/admin_update', False, first)[0] != 400:
            raise AssertionError('Anonymous camera update was not rejected')
        if request('/camera/admin_update', body={**first, 'csrf': 'invalid'})[0] != 400:
            raise AssertionError('Invalid-CSRF camera update was not rejected')
        if commands:
            raise AssertionError('Unauthorized request reached discovery')
        with ThreadPoolExecutor(max_workers=2) as pool:
            slow = pool.submit(request, '/camera/admin_update', body=first, timeout=15)
            try:
                if not entered.wait(8):
                    raise AssertionError('Loopback HTTP discovery did not start')
                independent = pool.submit(request, '/camera/admin_update', body=second, timeout=5)
                try:
                    second_result = independent.result(timeout=observation_seconds)
                    unblocked = True
                except TimeoutError:
                    second_result = None
                    unblocked = False
                if verify_stale_discovery:
                    disabled = {**first, 'ptzEnabled': 0}
                    if request('/camera/admin_update', body=disabled)[0] != 200:
                        raise AssertionError('Concurrent camera disable did not complete')
            finally:
                # Always free the transport before awaiting futures, including
                # failures. No worker or fake camera may leak into another test.
                release.set()
            first_result = slow.result(timeout=15)
            if second_result is None:
                second_result = independent.result(timeout=5)
        if first_result[0] != 200 or second_result[0] != 200:
            raise AssertionError('Fixture camera updates did not complete')
        stale_rejected = None
        if verify_stale_discovery:
            position_status = request('/ptz/1/position')[0]
            stale_rejected = position_status == 400
            if not stale_rejected:
                raise AssertionError(f'Stale discovery client rejection failed: position status {position_status}')
        if not commands or any(command != 'Login' for command in commands):
            raise AssertionError('Fixture was not limited to discovery login')
        return {'passed': unblocked, 'slowUpdateStatus': first_result[0],
                'independentUpdateStatus': second_result[0], 'observationSeconds': observation_seconds,
                'loginRequests': len(commands), 'motorCommands': 0,
                'staleDiscoveryRejected': stale_rejected,
                'scope': 'Loopback-only stalled PTZ login during actual camera updates; no physical transport'}
    finally:
        release.set()
        server.shutdown()
        worker.join(timeout=2)
        server.server_close()
