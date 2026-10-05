"""Opt-in full Windows server lifecycle smoke, not the 16-stream load gate.

Uses a newly created database, private cache/log/history, loopback binding and
fixture-owned normal sessions. Never copies live configuration or disables auth.
"""
import contextlib
import ctypes
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request


def stop_console_process(process):
    """Signal only this fixture's hidden console process group, then await exit."""
    if process.poll() is not None:
        return
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.AttachConsole.argtypes = [ctypes.c_uint32]
    kernel.GenerateConsoleCtrlEvent.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    # This Python process owns no user-facing UI; detach only its console.
    kernel.FreeConsole()
    if not kernel.AttachConsole(process.pid):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.GenerateConsoleCtrlEvent(1, process.pid):  # CTRL_BREAK_EVENT
            raise ctypes.WinError(ctypes.get_last_error())
        process.wait(timeout=30)
    finally:
        kernel.FreeConsole()


def loopback_port():
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        return reservation.getsockname()[1]


@contextlib.contextmanager
def replay_senders(inputs, urls, root):
    """Paced packet-copy senders; not independent physical camera/network feeds."""
    processes = []
    with contextlib.ExitStack() as files:
        try:
            for index, (source, url) in enumerate(zip(inputs, urls)):
                log = files.enter_context((root / f'replay-{index}.log').open('wb'))
                processes.append(subprocess.Popen(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'warning',
                    '-re', '-stream_loop', '-1', '-i', str(source), '-map', '0:v:0', '-map', '0:a?',
                    '-c', 'copy', '-f', 'mpegts', '-listen', '1', url], stdout=log, stderr=log,
                    creationflags=subprocess.CREATE_NO_WINDOW))
            yield processes
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('WITNESS_TEST_DATA_ROOT_SERVER'),
                     'Set WITNESS_TEST_DATA_ROOT_SERVER and WITNESS_TEST_DATA_ROOT_NATIVE_ID')
class FullServerIsolationTests(unittest.TestCase):
    def test_authenticated_health_and_history_survive_graceful_restart(self):
        server = Path(os.environ['WITNESS_TEST_DATA_ROOT_SERVER']).resolve(strict=True)
        identity = os.environ.get('WITNESS_TEST_DATA_ROOT_NATIVE_ID', '')
        self.assertRegex(identity, '^[0-9a-f]{12}$')
        binary = server.read_bytes()
        self.assertIn(identity.encode(), binary)
        self.assertIn('WITNESS_DATA_ROOT'.encode('utf-16-le'), binary)
        input_values = json.loads(os.environ.get('WITNESS_TEST_REPLAY_INPUTS', '[]'))
        self.assertIsInstance(input_values, list)
        self.assertLessEqual(len(input_values), 16)
        inputs = [Path(value).resolve(strict=True) for value in input_values]
        self.assertTrue(all(path.is_file() for path in inputs))
        replay_seconds = int(os.environ.get('WITNESS_TEST_REPLAY_SECONDS', '20'))
        self.assertTrue(10 <= replay_seconds <= 3600)
        urls = [f'http://127.0.0.1:{loopback_port()}/replay' for _ in inputs]
        repository = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix='full-server-isolation-', dir=repository / 'artifacts') as directory:
            root = Path(directory).resolve()
            data = root / 'private data'
            cache = root / 'private cache'; cache.mkdir()
            environment = dict(os.environ, WITNESS_DATA_ROOT=str(data))
            created = subprocess.run([str(server), '/createdb'], cwd=root, env=environment,
                                     capture_output=True, timeout=30)
            self.assertEqual(created.returncode, 0)
            port = loopback_port()
            token, csrf = secrets.token_hex(32), secrets.token_hex(32)
            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                db.execute('INSERT INTO User(Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin) VALUES(?,?,?,?,?,?)',
                           ('fixture-admin', 'Fixture admin', 'not-a-login-hash', 0, 1, 1))
                db.execute('INSERT INTO Session VALUES(?,?,?,?)', (token, csrf, 1, int(time.time())))
                settings = {'server_hostname': f'127.0.0.1:{port}', 'server_tls_mode': 'NoSecurity',
                            'server_cache': str(cache), 'clip_cleanup_enabled': '0', 'thread_count': '2',
                            'export_motion_vectors': '1'}
                db.executemany('INSERT OR REPLACE INTO Setting VALUES(?,?)', settings.items())
                self.assertEqual(db.execute('SELECT count(*) FROM Camera').fetchone()[0], 0)
                for index, url in enumerate(urls):
                    db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,MotionFilter,BlackoutMaskPath,FocusMaskPath) VALUES(?,?,?,?,?,?,?,?,?,?)',
                               (f'Fixture {index + 1}', url, '', 1, 1, 400, 0.99, 'motion_vectors', '', ''))
                db.commit()

            def request(path, authenticated=True, body=None):
                headers = {'Cookie': f'SessionToken-{port}={token}'} if authenticated else {}
                if body is not None:
                    headers['Content-Type'] = 'application/json'
                req = urllib.request.Request(f'http://127.0.0.1:{port}' + path,
                    data=None if body is None else json.dumps(body).encode(), headers=headers)
                try:
                    response = urllib.request.urlopen(req, timeout=3)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    return response.status, response.read()

            for run in range(2):
                startup = subprocess.STARTUPINFO()
                startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0  # Hidden dedicated console, not a user window.
                with replay_senders(inputs, urls, root) as senders, (root / f'run-{run}.log').open('wb') as log:
                    process = subprocess.Popen([str(server), '--plain-console'], cwd=root, env=environment,
                        stdout=log, stderr=subprocess.STDOUT, startupinfo=startup,
                        creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.CREATE_NEW_PROCESS_GROUP)
                    try:
                        deadline = time.monotonic() + 45
                        while True:
                            self.assertIsNone(process.poll(), 'server exited before readiness')
                            try:
                                code, body = request('/debug/health')
                                if code == 200:
                                    json.loads(body)
                                    break
                            except (OSError, urllib.error.URLError):
                                pass
                            self.assertLess(time.monotonic(), deadline, 'server readiness timeout')
                            time.sleep(0.1)
                        self.assertEqual(request('/debug/health', False)[0], 403)
                        self.assertEqual(request('/debug/client-health', False)[0], 403)
                        deadline = time.monotonic() + 10
                        while True:
                            code, body = request('/debug/client-health')
                            self.assertEqual(code, 200)
                            state = json.loads(body)
                            self.assertEqual(state['storageFailures'], 0, state)
                            if state['storageAvailable']:
                                break
                            self.assertLess(time.monotonic(), deadline, 'history storage readiness timeout')
                            time.sleep(0.05)
                        if run == 0:
                            payload = {'csrf': csrf, 'snapshot': {'browserId': 'isolated-browser',
                                'sessionId': 'isolated-session', 'visibilityState': 'visible', 'players': []}}
                            self.assertEqual(request('/debug/client-health', body=payload)[0], 202)
                        deadline = time.monotonic() + 10
                        while True:
                            code, body = request('/debug/client-health')
                            self.assertEqual(code, 200)
                            export = json.loads(body)
                            if len(export['history']) == 1 and export['durableThroughSequence'] >= 1:
                                break
                            self.assertLess(time.monotonic(), deadline, 'history durability timeout')
                            time.sleep(0.05)
                        self.assertEqual(export['history'][0]['username'], 'fixture-admin')
                        self.assertEqual(export['history'][0]['browserId'], 'isolated-browser')
                        if inputs:
                            deadline = time.monotonic() + 45
                            while True:
                                code, body = request('/debug/health')
                                self.assertEqual(code, 200)
                                health = json.loads(body)
                                cameras = health['cameras']
                                self.assertEqual(len(cameras), len(inputs))
                                if all(camera['streams'] and camera['streams'][0].get('acceptedVideoPackets', 0) > 5
                                       for camera in cameras):
                                    break
                                self.assertTrue(all(sender.poll() is None for sender in senders), 'replay sender exited')
                                if time.monotonic() >= deadline:
                                    (repository / 'artifacts/full-server-replay-readiness-failure.json').write_text(
                                        json.dumps(health, indent=2), encoding='utf-8')
                                    for source in (data / 'logs').glob('*.log'):
                                        shutil.copyfile(source, repository / 'artifacts/full-server-replay-readiness-failure.log')
                                    for index in range(min(2, len(senders))):
                                        shutil.copyfile(root / f'replay-{index}.log',
                                                        repository / f'artifacts/full-server-replay-sender-{index}.log')
                                self.assertLess(time.monotonic(), deadline, 'camera ingress readiness timeout')
                                time.sleep(0.25)
                            initial = {camera['cameraId']: camera['streams'][0]['acceptedVideoPackets'] for camera in cameras}
                            initial_processing = {camera['cameraId']: camera['processing']['completedFrames'] for camera in cameras}
                            samples = []
                            until = time.monotonic() + replay_seconds
                            while time.monotonic() < until:
                                started = time.monotonic()
                                code, body = request('/debug/health')
                                self.assertEqual(code, 200)
                                health = json.loads(body)
                                samples.append({'elapsedSeconds': replay_seconds - max(0, until - time.monotonic()),
                                    'httpMs': (time.monotonic() - started) * 1000,
                                    'server': health['server'], 'cameras': health['cameras']})
                                self.assertTrue(all(sender.poll() is None for sender in senders), 'replay sender exited')
                                time.sleep(1)
                            for camera in health['cameras']:
                                self.assertGreater(camera['streams'][0]['acceptedVideoPackets'], initial[camera['cameraId']],
                                                   f"camera {camera['cameraId']} stopped advancing")
                                self.assertGreater(camera['processing']['completedFrames'], initial_processing[camera['cameraId']],
                                                   f"camera {camera['cameraId']} processing stopped advancing")
                            (repository / f'artifacts/full-server-replay-run-{run}.json').write_text(
                                json.dumps({'nativeBuildId': identity, 'cameraCount': len(inputs),
                                    'limits': 'Paced loopback packet-copy replay; no browser clients or baseline phase; not the full load gate',
                                    'samples': samples}, indent=2), encoding='utf-8')
                    finally:
                        try:
                            stop_console_process(process)
                        finally:
                            if process.poll() is None:
                                process.kill()
                                process.wait(timeout=5)
                    if process.returncode != 0:
                        log.flush()
                        shutil.copyfile(root / f'run-{run}.log', repository / 'artifacts/full-server-isolation-failed-run.log')
                        for source in (data / 'logs').glob('*.log'):
                            shutil.copyfile(source, repository / 'artifacts/full-server-isolation-failed-server.log')
                    self.assertEqual(process.returncode, 0, 'graceful shutdown failed')
            self.assertTrue((data / 'client-health.db').is_file())
            self.assertFalse((root / 'server.db').exists())
            for path in root.rglob('*'):
                self.assertFalse(path.is_symlink() or path.is_junction())
                self.assertTrue(path.resolve().is_relative_to(root))


if __name__ == '__main__':
    unittest.main()
