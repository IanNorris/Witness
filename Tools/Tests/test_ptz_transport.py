"""Synthetic loopback camera; opt in with WITNESS_PTZ_TRANSPORT_PROBE."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import subprocess
import threading
import unittest
from quiet_subprocess import quiet_process_options


@unittest.skipUnless(os.environ.get('WITNESS_PTZ_TRANSPORT_PROBE'), 'native loopback probe not configured')
class PtzTransportTests(unittest.TestCase):
    def send(self, operation, replies):
        commands = []
        logins = []
        class Handler(BaseHTTPRequestHandler):
            def setup(self):
                super().setup()
                self.connection.settimeout(.5)
            def log_message(self, *args):
                pass
            def do_POST(self):
                try:
                    body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                except json.JSONDecodeError:
                    self.send_error(400, 'Malformed synthetic command'); return
                if body[0]['cmd'] == 'Login':
                    logins.append(body[0])
                    status, content = 200, '[{"cmd":"Login","code":0,"value":{"Token":{"name":"fixture-token"}}}]'
                else:
                    commands.append(body[0])
                    self.server.test.assertIn('token=fixture-token', self.path)
                    status, content = replies[min(len(commands) - 1, len(replies) - 1)]
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(content.encode())))
                self.end_headers(); self.wfile.write(content.encode())
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.test = self
        thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=.01), daemon=True); thread.start()
        try:
            result = subprocess.run([os.environ['WITNESS_PTZ_TRANSPORT_PROBE'], str(server.server_port), operation],
                capture_output=True, text=True, timeout=20, **quiet_process_options())
        finally:
            server.shutdown(); server.server_close(); thread.join(timeout=5)
        return result.returncode, logins if operation == 'discover' else commands

    def test_concurrent_discovery_reuses_one_authenticated_client(self):
        code, logins = self.send('discover', [])
        self.assertEqual(code, 0)
        self.assertEqual(len(logins), 1)

    def test_explicit_acknowledgement_and_wire_commands(self):
        for operation, expected in [('move', 'Left'), ('stop', 'Stop'), ('preset', 'ToPos')]:
            with self.subTest(operation=operation):
                code, commands = self.send(operation, [(200, '[{"cmd":"PtzCtrl","code":0}]')])
                self.assertEqual(code, 0)
                self.assertEqual(commands[0]['param']['op'], expected)
                self.assertEqual(commands[0]['param']['channel'], 0)
                if operation == 'move':
                    self.assertEqual(commands[0]['param']['speed'], 1)

    def test_missing_or_malformed_acknowledgement_is_not_success(self):
        for operation in ['move', 'stop', 'preset', 'absolute-zoom', 'absolute-focus', 'preset-save', 'preset-delete']:
            for response in ['[{}]', '[{"code":"0"}]', '[{"code":0.5}]', '[{"code":1e99}]', '[{"code":false}]',
                             '[null]', '["bad"]', '[{"code":0},{"code":0}]', '{}', 'not-json']:
                with self.subTest(operation=operation, response=response):
                    code, _ = self.send(operation, [(200, response)])
                    self.assertEqual(code, 3)

    def test_absolute_controls_and_preset_edits_use_valid_json(self):
        for operation in ['absolute-zoom', 'absolute-focus', 'preset-save', 'preset-delete']:
            with self.subTest(operation=operation):
                code, commands = self.send(operation, [(200, '[{"code":0}]')])
                self.assertEqual(code, 0)
                self.assertEqual(len(commands), 1)
                command = commands[0]
                if operation.startswith('absolute'):
                    self.assertEqual(command['cmd'], 'StartZoomFocus')
                    self.assertEqual(command['param']['ZoomFocus']['pos'], 1000)
                    self.assertEqual(command['param']['ZoomFocus']['op'], 'ZoomPos' if operation == 'absolute-zoom' else 'FocusPos')
                else:
                    self.assertEqual(command['cmd'], 'SetPtzPreset')
                    self.assertEqual(command['param']['id'], 1)
                    self.assertEqual(command['param']['enable'], int(operation == 'preset-save'))
                    if operation == 'preset-save':
                        self.assertEqual(command['param']['name'], 'Fixture "gate"\\position\n')

    def test_http_and_device_failure_are_not_acknowledgements(self):
        for operation in ['stop', 'absolute-zoom', 'absolute-focus', 'preset-save', 'preset-delete']:
            for status, response in [(500, '[{"code":0}]'), (200, '[{"code":-1}]')]:
                with self.subTest(operation=operation, status=status):
                    code, _ = self.send(operation, [(status, response)])
                    self.assertEqual(code, 3)

    def test_zoom_fallback_requires_explicit_success(self):
        for fallback, expected in [('[{"code":0}]', 0), ('[{}]', 3)]:
            code, commands = self.send('zoom', [(200, '[{"code":-1}]'), (200, fallback)])
            self.assertEqual(code, expected)
            self.assertEqual([c['param']['op'] for c in commands], ['ZoomInc', 'ZoomIn'])

    def test_stop_reauthenticates_once_and_retries_identical_command(self):
        code, commands = self.send('stop', [(401, '{}'), (200, '[{"code":0}]')])
        self.assertEqual(code, 0)
        self.assertEqual([c['param']['op'] for c in commands], ['Stop', 'Stop'])


if __name__ == '__main__':
    unittest.main()
