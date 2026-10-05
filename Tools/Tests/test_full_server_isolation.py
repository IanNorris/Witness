"""Opt-in full Windows server lifecycle smoke, not the 16-stream load gate.

Uses a newly created database, private cache/log/history, loopback binding and
fixture-owned normal sessions. Never copies live configuration or disables auth.
"""
import contextlib
import ctypes
import json
import os
import re
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
from check_preview_decode import inspect as inspect_recording


def sample_coverage(samples, duration, max_gap=10):
    """Reject suspended/partial observation windows, including missing endpoints."""
    times = [0.0] + [sample['elapsedSeconds'] for sample in samples] + [float(duration)]
    if not samples or any(not 0 <= value <= duration + max_gap for value in times):
        raise ValueError('Missing or out-of-range load samples')
    gaps = [right - left for left, right in zip(times, times[1:])]
    if any(gap < 0 or gap > max_gap for gap in gaps):
        raise ValueError('Load observation gap exceeds the continuous-test limit')
    return {'sampleCount': len(samples), 'durationSeconds': duration,
            'maximumGapSeconds': max(gaps), 'maximumAllowedGapSeconds': max_gap}


def browser_playback_ready(export, camera_ids, build_id, now_ms):
    """A fresh, authenticated real-player heartbeat, not synthetic setup telemetry."""
    expected_ids = {f'{camera_id}_mse' for camera_id in camera_ids}
    if not expected_ids:
        return False
    for row in export.get('history', []):
        if row.get('userUid') != 1 or row.get('username') != 'fixture-admin':
            continue
        received = row.get('receivedUnixMs')
        if type(received) is not int or not -1000 <= now_ms - received <= 15000:
            continue
        snapshot = row.get('snapshot', {})
        if (snapshot.get('buildHash') != build_id or snapshot.get('path') != '/' or
                snapshot.get('visibilityState') != 'visible'):
            continue
        players = snapshot.get('players', [])
        if not isinstance(players, list) or len(players) != len(expected_ids):
            continue
        if any(not isinstance(player, dict) for player in players):
            continue
        if {player.get('id') for player in players} != expected_ids:
            continue
        if all(type(player.get('readyState')) is int and player['readyState'] >= 3 and
               player.get('wsReadyState') == 1 and player.get('lastEventType') == 'media:playing' and
               type(player.get('totalVideoFrames')) is int and player['totalVideoFrames'] > 2 and
               player.get('errorCount') == 0 and player.get('hasInitialBuffer') is True and
               player.get('waitingForKeyframe') is False and player.get('awaitingInit') is False
               for player in players):
            return True
    return False


def inference_evidence(text):
    """Export only numeric completed-inference stats, never raw log/session data."""
    pattern = re.compile(r'ONNX Camera (\d+) stats: (\d+)/(\d+) detected '
                         r'\((\d+)%\), (\d+) skipped, (\d+) baseline-filtered, avg (\d+)ms')
    return [{'cameraId': int(match[0]), 'detections': int(match[1]),
             'completedInferences': int(match[2]), 'skipped': int(match[4]),
             'baselineFiltered': int(match[5]), 'meanInferenceMsRounded': int(match[6])}
            for match in pattern.findall(text) if int(match[2]) > 0]


def recording_evidence(db, previous_clip_uid, previous_segment_uid):
    """Only this lifecycle's newly created rows qualify; no reused baseline rows."""
    clips = [{'cameraId': row[0], 'clips': row[1]} for row in db.execute(
        'SELECT Camera,count(*) FROM Clip WHERE ClipUID>? GROUP BY Camera', (previous_clip_uid,))]
    segments = [{'cameraId': row[0], 'segments': row[1]} for row in db.execute(
        'SELECT CameraUID,count(*) FROM ContinuousSegment WHERE SegmentUID>? GROUP BY CameraUID',
        (previous_segment_uid,))]
    return clips, segments


def verify_recordings(db, cache, previous_clip_uid, previous_segment_uid):
    """Inspect only fresh fixture-owned local outputs, never arbitrary DB paths."""
    recordings = []
    for uid, camera, timestamp, mode in db.execute(
            'SELECT ClipUID,Camera,Timestamp,RecordMode FROM Clip WHERE ClipUID>?', (previous_clip_uid,)):
        if mode not in (0, 1):
            raise ValueError('Unexpected recording mode')
        recordings.append(('clip', camera, uid, cache / f'{int(camera)}_{"Manual" if mode == 0 else "Auto"}_{int(timestamp)}.mp4'))
    for uid, camera, path in db.execute(
            'SELECT SegmentUID,CameraUID,FilePath FROM ContinuousSegment WHERE SegmentUID>?', (previous_segment_uid,)):
        recordings.append(('continuous', camera, uid, Path(path)))
    reports = []
    for kind, camera, uid, path in recordings:
        path = path.resolve(strict=True)
        if not path.is_relative_to(cache.resolve()) or not path.is_file():
            raise ValueError('Recording is outside the private cache')
        result = inspect_recording(path)
        result.pop('file', None)
        reports.append(dict(result, kind=kind, cameraId=camera, recordingId=uid))
    return reports


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
        browser_hold = int(os.environ.get('WITNESS_TEST_BROWSER_HOLD_SECONDS', '0'))
        self.assertTrue(browser_hold == 0 or 60 <= browser_hold <= 3600)
        self.assertTrue(not browser_hold or inputs, 'Browser mode requires replay inputs')
        active_detection = os.environ.get('WITNESS_TEST_ACTIVE_DETECTION') == '1'
        active_recording = active_detection or os.environ.get('WITNESS_TEST_RECORDING') == '1'
        verify_outputs = os.environ.get('WITNESS_TEST_VERIFY_RECORDINGS') == '1'
        self.assertTrue(not verify_outputs or active_recording, 'Output verification requires recording mode')
        lifecycle_runs = int(os.environ.get('WITNESS_TEST_LIFECYCLE_RUNS', '2'))
        self.assertTrue(1 <= lifecycle_runs <= 10)
        self.assertTrue(not active_recording or inputs, 'Recording mode requires replay inputs')
        self.assertTrue(not active_detection or (inputs and (browser_hold or replay_seconds) >= 90),
                        'Detection evidence needs replay inputs and at least 90 seconds')
        if active_detection:
            self.assertTrue((server.parent / 'models/yolo26n.onnx').is_file())
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
            executable = server
            if browser_hold:
                # Alter only a private copy, never the verified deployment/build.
                runtime = root / 'private runtime'
                shutil.copytree(server.parent, runtime)
                executable = runtime / server.name
                index = runtime / 'Web/index.html'
                bootstrap = '<script>document.cookie=' + json.dumps(f'SessionToken-{port}={token}; Path=/; SameSite=Strict') + ';'
                bootstrap += "localStorage.setItem('streamingMode','\"mse\"');localStorage.setItem('fullscreenMode','false');"
                bootstrap += "localStorage.setItem('witness-dashboard-windowed-mode','layout');"
                bootstrap += '</script>'
                index.write_text(index.read_text(encoding='utf-8').replace('<head>', '<head>' + bootstrap, 1), encoding='utf-8')
            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                db.execute('INSERT INTO User(Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin) VALUES(?,?,?,?,?,?)',
                           ('fixture-admin', 'Fixture admin', 'not-a-login-hash', 0, 1, 1))
                db.execute('INSERT INTO Session VALUES(?,?,?,?)', (token, csrf, 1, int(time.time())))
                settings = {'server_hostname': f'127.0.0.1:{port}', 'server_tls_mode': 'NoSecurity',
                            'server_cache': str(cache), 'clip_cleanup_enabled': '0', 'thread_count': '2',
                            'export_motion_vectors': '1'}
                if active_detection:
                    settings.update({'detection_backend': 'onnx', 'detection_provider': 'cpu',
                                     'detection_max_fps': '1'})
                db.executemany('INSERT OR REPLACE INTO Setting VALUES(?,?)', settings.items())
                self.assertEqual(db.execute('SELECT count(*) FROM Camera').fetchone()[0], 0)
                for index, url in enumerate(urls):
                    db.execute('INSERT INTO Camera(CameraName,CameraString,CameraStringSub,Enabled,SkipFrames,MDFrameHeight,MDThreshold,MotionFilter,BlackoutMaskPath,FocusMaskPath,ContinuousRecording) VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                               (f'Fixture {index + 1}', url, '', 1, 1, 400,
                                0.01 if active_detection else 0.99, 'motion_vectors', '', '', int(active_recording)))
                if browser_hold or active_recording:
                    db.execute('INSERT INTO CameraGroup(DisplayName,Description) VALUES(?,?)', ('Isolated replay', 'Test only'))
                    db.execute('INSERT INTO UserGroupMapping VALUES(?,?)', (1, 1))
                    db.executemany('INSERT INTO CameraGroupMapping VALUES(?,?)', [(index + 1, 1) for index in range(len(inputs))])
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

            for run in range(1 if browser_hold else lifecycle_runs):
                log_offsets = {path: path.stat().st_size for path in (data / 'logs').glob('*.log')}
                with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                    previous_clip_uid = db.execute('SELECT coalesce(max(ClipUID),0) FROM Clip').fetchone()[0]
                    previous_segment_uid = db.execute('SELECT coalesce(max(SegmentUID),0) FROM ContinuousSegment').fetchone()[0]
                startup = subprocess.STARTUPINFO()
                startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0  # Hidden dedicated console, not a user window.
                with replay_senders(inputs, urls, root) as senders, (root / f'run-{run}.log').open('wb') as log:
                    process = subprocess.Popen([str(executable), '--plain-console'], cwd=root, env=environment,
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
                                'sessionId': 'isolated-session', 'visibilityState': 'visible', 'players': [{
                                    'id': 'fixture_mse', 'lastDriftRestart': {
                                        'type': 'driftRestart', 'reason': 'drift', 'lagMs': 148000,
                                        'message': 'https://secret@camera',
                                        'drift': {'paused': True, 'fragmentAgeMs': 76,
                                                  'buffered': [[193, 222]], 'password': 'secret'}}}]}}
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
                        # Must survive the real authenticated endpoint, asynchronous storage
                        # and server restart; arbitrary nested payloads must not survive.
                        recovery = export['history'][0]['snapshot']['players'][0]['lastDriftRestart']
                        self.assertEqual(recovery['lagMs'], 148000)
                        self.assertEqual(recovery['reason'], 'drift')
                        self.assertEqual(recovery['drift']['fragmentAgeMs'], 76)
                        self.assertTrue(recovery['drift']['paused'])
                        self.assertNotIn('buffered', recovery['drift'])
                        self.assertNotIn('secret', json.dumps(recovery))
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
                            if active_recording:
                                # Normal authenticated recording path, not manufactured DB rows.
                                # These are explicit manual clips, not proof of motion activation.
                                for camera_id in initial:
                                    self.assertEqual(request(f'/camera/record/{camera_id}',
                                        body={'csrf': csrf, 'record': True})[0], 200)
                            if browser_hold:
                                print(f'isolatedBrowserUrl=http://127.0.0.1:{port}/', flush=True)
                                # Do not count browser attachment/startup as measured playback.
                                # Independent DOM observations remain required in addition to this gate.
                                deadline = time.monotonic() + 300
                                while True:
                                    self.assertIsNone(process.poll(), 'server exited before browser playback')
                                    self.assertTrue(all(sender.poll() is None for sender in senders), 'replay sender exited')
                                    code, history = request('/debug/client-health')
                                    self.assertEqual(code, 200)
                                    if browser_playback_ready(json.loads(history), initial.keys(),
                                            health['server']['webBuildHash'], int(time.time() * 1000)):
                                        print('browserPlaybackReady=true', flush=True)
                                        break
                                    self.assertLess(time.monotonic(), deadline, 'browser playback readiness timeout')
                                    time.sleep(1)
                                code, phase_body = request('/debug/health')
                                self.assertEqual(code, 200)
                                phase_health = json.loads(phase_body)
                                self.assertEqual(len(phase_health['cameras']), len(inputs))
                                initial = {camera['cameraId']: camera['streams'][0]['acceptedVideoPackets']
                                           for camera in phase_health['cameras']}
                                initial_processing = {camera['cameraId']: camera['processing']['completedFrames']
                                                      for camera in phase_health['cameras']}
                            samples = []
                            duration = browser_hold or replay_seconds
                            until = time.monotonic() + duration
                            while time.monotonic() < until:
                                started = time.monotonic()
                                code, body = request('/debug/health')
                                self.assertEqual(code, 200)
                                health = json.loads(body)
                                samples.append({'elapsedSeconds': duration - max(0, until - time.monotonic()),
                                    'httpMs': (time.monotonic() - started) * 1000,
                                    'server': health['server'], 'cameras': health['cameras']})
                                self.assertTrue(all(sender.poll() is None for sender in senders), 'replay sender exited')
                                time.sleep(1)
                            if browser_hold:
                                code, history = request('/debug/client-health')
                                self.assertEqual(code, 200)
                                (repository / 'artifacts/full-server-browser-history.json').write_bytes(history)
                            coverage = sample_coverage(samples, duration)
                            if active_recording:
                                for camera_id in initial:
                                    self.assertEqual(request(f'/camera/record/{camera_id}',
                                        body={'csrf': csrf, 'record': False})[0], 200)
                            for camera in health['cameras']:
                                self.assertGreater(camera['streams'][0]['acceptedVideoPackets'], initial[camera['cameraId']],
                                                   f"camera {camera['cameraId']} stopped advancing")
                                self.assertGreater(camera['processing']['completedFrames'], initial_processing[camera['cameraId']],
                                                   f"camera {camera['cameraId']} processing stopped advancing")
                                if os.environ.get('WITNESS_TEST_PROCESSING_LATENCY') == '1':
                                    work = camera['processing']['workLatency']
                                    self.assertTrue(0 < work['samples'] <= work['capacity'] == 1024)
                                    self.assertGreater(work['samplesSinceReset'], 0)
                                    self.assertTrue(0 <= work['p50Ms'] <= work['p95Ms'] <= work['p99Ms'] <= work['maxMs'])
                                    self.assertGreater(work['maxMs'], 0)
                                    self.assertEqual(work['scope'], 'filterStageWallTimeExcludingQueueWaitAndDecode')
                            if os.environ.get('WITNESS_TEST_PROCESSING_LATENCY') == '1':
                                before_work = {camera['cameraId']: camera['processing']['workLatency'] for camera in health['cameras']}
                                self.assertEqual(request('/camera/admin_reset_stats', body={'csrf': csrf})[0], 200)
                                code, reset_body = request('/debug/health')
                                self.assertEqual(code, 200)
                                reset_health = json.loads(reset_body)
                                for camera in reset_health['cameras']:
                                    work = camera['processing']['workLatency']
                                    before = before_work[camera['cameraId']]
                                    self.assertEqual(work['epoch'], before['epoch'] + 1)
                                    self.assertLess(work['samplesSinceReset'], before['samplesSinceReset'])
                                    self.assertLessEqual(work['samples'], work['samplesSinceReset'])
                                    if not work['samples']:
                                        self.assertIsNone(work['p95Ms'])
                            (repository / f'artifacts/full-server-replay-run-{run}.json').write_text(
                                json.dumps({'nativeBuildId': identity, 'cameraCount': len(inputs),
                                    'limits': 'Paced loopback packet-copy replay; browser attachment is externally verified; no baseline phase; not the full load gate',
                                    'coverage': coverage,
                                    'browserReadyBeforePhase': bool(browser_hold),
                                    'workload': {'objectDetection': active_detection, 'provider': 'cpu' if active_detection else None,
                                                 'maximumMotionInferenceFps': 1 if active_detection else None,
                                                 'recording': 'continuous DVR plus authenticated manual clips' if active_recording else 'disabled',
                                                 'strictRecordingInspection': verify_outputs,
                                                 'motionThreshold': 0.01 if active_detection else 0.99},
                                    'samples': samples}, indent=2), encoding='utf-8')
                    finally:
                        try:
                            stop_console_process(process)
                        except BaseException:
                            # Preserve a failing lifecycle before private fixture cleanup.
                            log.flush()
                            shutil.copyfile(root / f'run-{run}.log', repository / 'artifacts/full-server-isolation-failed-run.log')
                            for source in (data / 'logs').glob('*.log'):
                                shutil.copyfile(source, repository / 'artifacts/full-server-isolation-failed-server.log')
                            debugger = Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
                            if process.poll() is None and debugger.is_file():
                                try:
                                    subprocess.run([str(debugger), '-pv', '-p', str(process.pid),
                                        '-y', str(server.parent), '-logo', str(repository / 'artifacts/full-server-shutdown-hang-stacks.log'),
                                        '-c', '~* kb; .detach; q'], stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, timeout=20,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
                                except (OSError, subprocess.TimeoutExpired):
                                    pass  # Diagnostics never prevent test-owned process cleanup.
                            raise
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
                    if active_recording:
                        log.flush()
                        evidence = inference_evidence((root / f'run-{run}.log').read_text(encoding='utf-8', errors='replace'))
                        for source in (data / 'logs').glob('*.log'):
                            with source.open('rb') as stream:
                                stream.seek(log_offsets.get(source, 0))
                                evidence.extend(inference_evidence(stream.read().decode('utf-8', errors='replace')))
                        # Log sinks can duplicate lines; counts are evidence windows, not lifetime totals.
                        evidence = list({json.dumps(row, sort_keys=True): row for row in evidence}.values())
                        with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                            clips, segments = recording_evidence(db, previous_clip_uid, previous_segment_uid)
                            decoded = verify_recordings(db, cache, previous_clip_uid, previous_segment_uid) if verify_outputs else []
                        (repository / f'artifacts/full-server-workload-run-{run}.json').write_text(
                            json.dumps({'completedInferenceWindows': evidence, 'clipRows': clips,
                                        'continuousSegmentRows': segments,
                                        'recordingInspection': decoded,
                                        'limits': 'Rows alone prove creation; optional inspection checks strict decode and packet/frame timestamps, not perceptual smoothness; clips are manually triggered, not proof of MV activation; inferred frames include 30-second no-motion baselines'}, indent=2), encoding='utf-8')
                        if active_detection:
                            self.assertEqual({row['cameraId'] for row in evidence}, set(range(1, len(inputs) + 1)),
                                             'Every camera must complete real inference, not just load a model')
                        self.assertEqual({row['cameraId'] for row in clips}, set(range(1, len(inputs) + 1)),
                                         'Every camera must create a new recorded clip')
                        self.assertEqual({row['cameraId'] for row in segments}, set(range(1, len(inputs) + 1)),
                                         'Every camera must create new continuous DVR segments')
                        if verify_outputs:
                            self.assertTrue(decoded and all(row['passed'] for row in decoded),
                                            'Strict recording decode/timestamp inspection failed')
            self.assertTrue((data / 'client-health.db').is_file())
            self.assertFalse((root / 'server.db').exists())
            for path in root.rglob('*'):
                self.assertFalse(path.is_symlink() or path.is_junction())
                self.assertTrue(path.resolve().is_relative_to(root))


if __name__ == '__main__':
    unittest.main()
