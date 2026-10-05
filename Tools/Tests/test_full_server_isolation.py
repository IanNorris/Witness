"""Opt-in full Windows server lifecycle smoke, not the 16-stream load gate.

Uses a newly created database, private cache/log/history, loopback binding and
fixture-owned normal sessions. Never copies live configuration or disables auth.
"""
import contextlib
import gzip
import ctypes
import json
import math
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
from check_long_recording import inspect_long
from owned_process_dump import capture_owned_process_dump
from processing_sample_collector import ProcessingSampleCollector
from health_request_timing import sample_health
from replay_profiles import qualify_profiles, validate_live_profiles, validate_browser_layout_proof
from ptz_update_scope_fixture import check_update_scope
from ptz_http_relink_fixture import check_http_relink
from multi_client_history_fixture import check_multi_client_history

RECORDING_FAILURE_ROOT = Path(__file__).resolve().parents[2] / 'artifacts'


def artifact_directory(repository, namespace=None):
    """Opt-in exclusive run directory; never overwrite another invocation's evidence."""
    root = Path(repository) / 'artifacts'
    root.mkdir(exist_ok=True)
    if root.is_symlink() or root.is_junction() or not root.is_dir():
        raise ValueError('Artifact root must be an unlinked directory')
    if namespace is None:
        return root
    if not isinstance(namespace, str) or not re.fullmatch('[a-z0-9][a-z0-9-]{0,63}', namespace):
        raise ValueError('Invalid artifact namespace')
    target = root / ('run-' + namespace)
    target.mkdir()  # Exclusive: existing names, including links, fail closed.
    return target


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


def browser_playback_ready(export, camera_ids, build_id, now_ms, require_presentation=False):
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
        def progressing(player):
            samples = player.get('driftSamples', [])
            if not isinstance(samples, list) or not samples or not isinstance(samples[-1], dict):
                return False
            drift = samples[-1]
            if require_presentation:
                frame_count = drift.get('presentedFrames')
                clock, presented = drift.get('currentTime'), drift.get('presentedMediaTime')
                if (drift.get('presentationVisible') is not True or type(frame_count) is not int or
                        frame_count < 2 or any(type(value) not in (int, float) or not math.isfinite(value)
                                              for value in (clock, presented)) or abs(clock-presented) > 1):
                    return False
            advance = drift.get('playbackAdvanceMs')
            interval = drift.get('sampleIntervalMs')
            return (drift.get('visibility') == 'visible' and drift.get('paused') is False and
                    drift.get('seeking') is not True and type(advance) in (int, float) and
                    type(interval) in (int, float) and 0 < advance <= 15000 and 0 < interval <= 15000)

        if all(type(player.get('readyState')) is int and player['readyState'] >= 3 and
               player.get('wsReadyState') == 1 and progressing(player) and
               player.get('lastEventType') not in ('media:waiting', 'media:stalled', 'media:pause', 'media:ended', 'media:error') and
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
    clips = [{'cameraId': row[0], 'clips': row[1], 'minimumDurationSeconds': row[2],
              'maximumDurationSeconds': row[3], 'invalidMotionDurationCount': row[4]} for row in db.execute(
        'SELECT Camera,count(*),min(Duration),max(Duration),'
        'sum(CASE WHEN ActiveDuration IS NULL OR ActiveDuration<0 OR ActiveDuration>Duration THEN 1 ELSE 0 END) '
        'FROM Clip WHERE ClipUID>? GROUP BY Camera', (previous_clip_uid,))]
    segments = [{'cameraId': row[0], 'segments': row[1]} for row in db.execute(
        'SELECT CameraUID,count(*) FROM ContinuousSegment WHERE SegmentUID>? GROUP BY CameraUID',
        (previous_segment_uid,))]
    return clips, segments


def recording_metadata_finalized(clips, maximum_duration_seconds):
    return bool(clips) and all(type(row.get('minimumDurationSeconds')) is int and
                              type(row.get('maximumDurationSeconds')) is int and
                              type(row.get('invalidMotionDurationCount')) is int and
                              row['invalidMotionDurationCount'] == 0 and
                              0 < row['minimumDurationSeconds'] <= row['maximumDurationSeconds'] <=
                              maximum_duration_seconds for row in clips)


def replay_sender_failures(processes, root, inputs, urls):
    """Bounded evidence from this fixture's owned senders, not server/session logs."""
    failures = []
    for index, process in enumerate(processes):
        code = process.poll()
        if code is None:
            continue
        path = root / f'replay-{index}.log'
        with path.open('rb') as source:
            source.seek(0, 2)
            source.seek(max(0, source.tell() - 16384))
            tail = source.read(16384).decode('utf-8', errors='replace')
        for private in (str(root), str(inputs[index]), urls[index]):
            tail = tail.replace(private, '[fixture]')
        failures.append({'cameraId': index + 1, 'returnCode': code, 'diagnostics': tail})
    return failures


def verify_recordings(db, cache, previous_clip_uid, previous_segment_uid, progress_path=None, failure_root=None,
                      frame_decoder_threads=1):
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
    def write_progress(complete):
        if progress_path is None:
            return
        target = Path(progress_path)
        temporary = target.with_suffix('.tmp')
        temporary.write_text(json.dumps({
            'schemaVersion': 1, 'inspectionComplete': complete,
            'expectedOutputs': len(recordings), 'completedOutputs': len(reports),
            'passed': complete and bool(reports) and all(row['passed'] for row in reports),
            'recordingInspection': reports}, indent=2), encoding='utf-8')
        temporary.replace(target)
    write_progress(False)
    for kind, camera, uid, path in recordings:
        path = path.resolve(strict=True)
        if not path.is_relative_to(cache.resolve()) or not path.is_file():
            raise ValueError('Recording is outside the private cache')
        try:
            result = (inspect_long(path, frame_decoder_threads=frame_decoder_threads)
                      if os.environ.get('WITNESS_TEST_LONG_RECORDING_INSPECTION') == '1'
                      else inspect_recording(path))
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            # Keep only this already validated, fixture-owned bounded recording.
            # Never preserve a live DB/session, external path, or raw command line.
            evidence = (Path(failure_root) if failure_root is not None else RECORDING_FAILURE_ROOT) / ('recording-inspection-failed-' + secrets.token_hex(8))
            saved = evidence.with_suffix('.mp4')
            if path.stat().st_size <= 512 * 1024 * 1024:
                with path.open('rb') as source, saved.open('xb') as destination:
                    shutil.copyfileobj(source, destination)
            details = {
                'passed': False, 'kind': kind, 'cameraId': camera, 'recordingId': uid,
                'nativeBuildId': os.environ.get('WITNESS_TEST_DATA_ROOT_NATIVE_ID'),
                'httpTraceMode': os.environ.get('WITNESS_TEST_HTTP_TRACE_MODE'),
                'sqliteTraceMode': os.environ.get('WITNESS_TEST_SQLITE_TRACE_MODE'),
                'stage': getattr(error, 'stage', None),
                'returnCode': getattr(error, 'return_code', None),
                'errorType': type(error).__name__,
                'diagnostics': getattr(error, 'diagnostics', ''),
                'savedRecording': saved.name if saved.is_file() else None,
            }
            with evidence.with_suffix('.json').open('x', encoding='utf-8') as destination:
                json.dump(details, destination, indent=2)
            print('recordingInspectionFailure=' + str(evidence.with_suffix('.json')), flush=True)
            raise
        result.pop('file', None)
        reports.append(dict(result, kind=kind, cameraId=camera, recordingId=uid))
        write_progress(False)
    write_progress(True)
    return reports


def wait_for_console_exit(process, slow_shutdown=None):
    """Keep the original deadline; optional evidence is captured while attached."""
    deadline = time.monotonic() + 30
    try:
        process.wait(timeout=10 if slow_shutdown is not None else 30)
    except subprocess.TimeoutExpired:
        if slow_shutdown is None:
            raise
        slow_shutdown()
        process.wait(timeout=max(0, deadline - time.monotonic()))


def stop_console_process(process, slow_shutdown=None):
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
        wait_for_console_exit(process, slow_shutdown)
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
                    '-c', 'copy', '-f', 'mpegts', '-listen', '1', url], stdin=subprocess.DEVNULL,
                    stdout=log, stderr=log,
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
        expected_packet_trace = os.environ.get('WITNESS_TEST_PACKET_TRACE_MODE')
        self.assertIn(expected_packet_trace, (None, '0', '1'))
        expected_mux_trace = os.environ.get('WITNESS_TEST_MUX_TRACE_MODE')
        self.assertIn(expected_mux_trace, (None, '0', '1'))
        expected_media_trace = os.environ.get('WITNESS_TEST_MEDIA_TRACE_MODE')
        self.assertIn(expected_media_trace, (None, '0', '1'))
        expected_server_traces = {
            'http': os.environ.get('WITNESS_TEST_HTTP_TRACE_MODE'),
            'database': os.environ.get('WITNESS_TEST_SQLITE_TRACE_MODE'),
        }
        for expected in expected_server_traces.values():
            self.assertIn(expected, (None, '0', '1'))
        self.assertRegex(identity, '^[0-9a-f]{12}$')
        binary = server.read_bytes()
        self.assertIn(identity.encode(), binary)
        self.assertIn('WITNESS_DATA_ROOT'.encode('utf-16-le'), binary)
        input_values = json.loads(os.environ.get('WITNESS_TEST_REPLAY_INPUTS', '[]'))
        self.assertIsInstance(input_values, list)
        self.assertLessEqual(len(input_values), 16)
        inputs = [Path(value).resolve(strict=True) for value in input_values]
        self.assertTrue(all(path.is_file() for path in inputs))
        expected_profiles = os.environ.get('WITNESS_TEST_EXPECTED_REPLAY_PROFILES')
        profiles = qualify_profiles(inputs, json.loads(expected_profiles)) if expected_profiles else None
        replay_seconds = int(os.environ.get('WITNESS_TEST_REPLAY_SECONDS', '20'))
        self.assertTrue(10 <= replay_seconds <= 3600)
        browser_hold = int(os.environ.get('WITNESS_TEST_BROWSER_HOLD_SECONDS', '0'))
        viewport_option = os.environ.get('WITNESS_TEST_BROWSER_GATE_VIEWPORT')
        browser_gate_viewport = json.loads(viewport_option) if viewport_option else None
        if browser_gate_viewport is not None:
            self.assertTrue(browser_hold and profiles is not None)
            self.assertEqual(set(browser_gate_viewport), {'width', 'height'})
            self.assertTrue(all(type(value) is int and 1 <= value <= 10000
                                for value in browser_gate_viewport.values()))
        self.assertTrue(browser_hold == 0 or 60 <= browser_hold <= 3600)
        self.assertTrue(not browser_hold or inputs, 'Browser mode requires replay inputs')
        active_detection = os.environ.get('WITNESS_TEST_ACTIVE_DETECTION') == '1'
        active_recording = active_detection or os.environ.get('WITNESS_TEST_RECORDING') == '1'
        verify_outputs = os.environ.get('WITNESS_TEST_VERIFY_RECORDINGS') == '1'
        frame_decoder_threads = int(os.environ.get('WITNESS_TEST_RECORDING_FRAME_THREADS', '1'))
        self.assertTrue(1 <= frame_decoder_threads <= 8)
        raw_processing = os.environ.get('WITNESS_TEST_RAW_PROCESSING_SAMPLES') == '1'
        cached_health = os.environ.get('WITNESS_TEST_CACHED_HEALTH') == '1'
        multi_client_history = os.environ.get('WITNESS_TEST_MULTI_CLIENT_HISTORY') == '1'
        self.assertTrue(not multi_client_history or (not inputs and not browser_hold),
                        'Cross-client auth fixture is a separate zero-camera lifecycle test')
        ptz_guard = os.environ.get('WITNESS_TEST_PTZ_CONFIGURED_GUARD') == '1'
        ptz_update_scope = os.environ.get('WITNESS_TEST_PTZ_UPDATE_SCOPE') == '1'
        ptz_http_relink = os.environ.get('WITNESS_TEST_PTZ_HTTP_RELINK') == '1'
        self.assertTrue(not ptz_http_relink or len(inputs) >= 3,
                        'HTTP PTZ relink checks need three disposable cameras')
        self.assertTrue(not ptz_update_scope or len(inputs) >= 2,
                        'PTZ update scope checks need two disposable replay cameras')
        self.assertTrue(not ptz_guard or inputs, 'PTZ guard checks require a configured synthetic camera')
        self.assertTrue(not verify_outputs or active_recording, 'Output verification requires recording mode')
        lifecycle_runs = int(os.environ.get('WITNESS_TEST_LIFECYCLE_RUNS', '2'))
        self.assertTrue(1 <= lifecycle_runs <= 10)
        self.assertTrue(not multi_client_history or lifecycle_runs >= 2,
                        'Cross-client durability requires a real server restart')
        self.assertTrue(not active_recording or inputs, 'Recording mode requires replay inputs')
        self.assertTrue(not active_detection or (inputs and (browser_hold or replay_seconds) >= 90),
                        'Detection evidence needs replay inputs and at least 90 seconds')
        if active_detection:
            self.assertTrue((server.parent / 'models/yolo26n.onnx').is_file())
        urls = [f'http://127.0.0.1:{loopback_port()}/replay' for _ in inputs]
        repository = Path(__file__).resolve().parents[2]
        artifacts = artifact_directory(repository, os.environ.get('WITNESS_TEST_ARTIFACT_NAMESPACE'))
        with tempfile.TemporaryDirectory(prefix='full-server-isolation-', dir=artifacts) as directory:
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
                principals = {}
                if multi_client_history:
                    for name in ('alice', 'bob', 'exporter'):
                        uid = db.execute('INSERT INTO User(Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin) VALUES(?,?,?,?,?,?)',
                            ('fixture-' + name, 'Fixture ' + name, 'not-a-login-hash', 0, 1, int(name == 'exporter'))).lastrowid
                        principals[name] = {'uid': uid, 'token': secrets.token_hex(32), 'csrf': secrets.token_hex(32)}
                        db.execute('INSERT INTO Session VALUES(?,?,?,?)',
                            (principals[name]['token'], principals[name]['csrf'], uid, int(time.time())))
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
                if browser_hold or active_recording or ptz_guard or ptz_update_scope or ptz_http_relink:
                    db.execute('INSERT INTO CameraGroup(DisplayName,Description) VALUES(?,?)', ('Isolated replay', 'Test only'))
                    db.execute('INSERT INTO UserGroupMapping VALUES(?,?)', (1, 1))
                    db.executemany('INSERT INTO CameraGroupMapping VALUES(?,?)', [(index + 1, 1) for index in range(len(inputs))])
                db.commit()

            def request(path, authenticated=True, body=None, timeout=3, token_override=None):
                if raw_processing and path == '/debug/health':
                    path += '?workSamples=1'
                headers = {'Cookie': f'SessionToken-{port}={token if token_override is None else token_override}'} if authenticated else {}
                if body is not None:
                    headers['Content-Type'] = 'application/json'
                req = urllib.request.Request(f'http://127.0.0.1:{port}' + path,
                    data=None if body is None else json.dumps(body).encode(), headers=headers)
                try:
                    response = urllib.request.urlopen(req, timeout=timeout)
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
                        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, startupinfo=startup,
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
                        if ptz_http_relink:
                            report = check_http_relink(request, csrf, urls, data / 'server.db')
                            with (artifacts / f'full-server-ptz-http-relink-run-{run}.json').open('x', encoding='utf8') as output:
                                json.dump({'nativeBuildId': identity, **report}, output, indent=2)
                            self.assertTrue(report['passed'])
                        if ptz_update_scope:
                            report = check_update_scope(request, csrf, urls,
                                verify_stale_discovery=os.environ.get('WITNESS_TEST_PTZ_STALE_DISCOVERY') == '1')
                            with (artifacts / f'full-server-ptz-update-scope-run-{run}.json').open('x', encoding='utf8') as output:
                                json.dump({'nativeBuildId': identity, **report}, output, indent=2)
                            self.assertTrue(report['passed'], 'Stalled PTZ discovery retained UpdateCamera query mutex')
                        if cached_health:
                            self.assertEqual(request('/debug/health?details=bad', False)[0], 403)
                            self.assertEqual(request('/debug/health?details=bad')[0], 400)
                            snapshots = []
                            for _ in range(8):
                                code, summary = request('/debug/health?workSamples=0')
                                self.assertEqual(code, 200)
                                summary = json.loads(summary)
                                self.assertEqual(summary['server']['collectionMode'], 'cachedSummary')
                                self.assertFalse(summary['coverage']['incidentDetailsIncluded'])
                                self.assertEqual(summary['cache']['intervalMs'], 2000)
                                self.assertGreaterEqual(summary['cache']['ageMs'], 0)
                                self.assertFalse(summary['cache']['stale'])
                                self.assertEqual(summary['cache']['failedCollections'], 0)
                                self.assertFalse(summary['server']['http']['recentSlowRequests'])
                                self.assertFalse(summary['server']['http']['activeRequests'])
                                self.assertFalse(summary['server']['database']['recentSlowQueries'])
                                self.assertTrue(all(not stream.get('recentMediaEvents', [])
                                    for camera in summary['cameras'] for stream in camera['streams']))
                                snapshots.append(summary['server']['requestSequence'])
                            self.assertLess(len(set(snapshots)), len(snapshots),
                                            'Requests appear to recollect instead of reuse snapshots')
                            deadline = time.monotonic() + 6
                            while summary['server']['requestSequence'] == snapshots[-1]:
                                self.assertLess(time.monotonic(), deadline, 'background sampler stopped')
                                time.sleep(0.1)
                                code, body = request('/debug/health?workSamples=0')
                                self.assertEqual(code, 200)
                                summary = json.loads(body)
                            code, detail = request('/debug/health?details=1')
                            self.assertEqual(code, 200)
                            detail = json.loads(detail)
                            self.assertEqual(detail['server']['collectionMode'], 'onDemand')
                            self.assertTrue(detail['coverage']['incidentDetailsIncluded'])
                            self.assertNotIn('cache', detail)
                            # Private fixture database only: a valid populated
                            # cache must not bypass live account revocation.
                            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                                db.execute('UPDATE User SET Enabled=0 WHERE Username=?', ('fixture-admin',))
                                db.commit()
                            self.assertEqual(request('/debug/health?workSamples=0')[0], 403)
                            self.assertEqual(request('/debug/health?details=1')[0], 403)
                            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                                db.execute('UPDATE User SET Enabled=1 WHERE Username=?', ('fixture-admin',))
                                db.commit()
                            self.assertEqual(request('/debug/health?workSamples=0')[0], 200)
                            with (artifacts / f'full-server-cached-health-run-{run}.json').open('x', encoding='utf8') as output:
                                json.dump({'nativeBuildId': identity, 'passed': True,
                                           'requestSequences': snapshots,
                                           'nextSampleSequence': summary['server']['requestSequence'],
                                           'revokedCachedStatus': 403, 'detailsMode': 'onDemand',
                                           'scope': 'Actual authenticated Crow requests, cache reuse, background advancement and fresh account checks; no API key revocation or overload/stale injection'}, output, indent=2)
                        if ptz_guard:
                            # Every camera in this fixture has PTZ disabled and
                            # only loopback media endpoints. Never enable/create
                            # a PTZ transport or issue a physical camera command.
                            with contextlib.closing(sqlite3.connect(data / 'server.db')) as db:
                                self.assertEqual(db.execute('SELECT coalesce(PtzEnabled,0) FROM Camera WHERE CameraUID=1').fetchone()[0], 0)
                            results = []
                            for path, fields in [('/ptz/1/left', {'speed': 2}),
                                                 ('/ptz/1/preset', {'id': 1}),
                                                 ('/ptz/1/zoom/set', {'zoom': 1000}),
                                                 ('/ptz/1/preset/set', {'id': 1, 'name': 'fixture'}),
                                                 ('/ptz/1/preset/delete', {'id': 1})]:
                                body = {'csrf': csrf, **fields}
                                anonymous = request(path, False, body)[0]
                                invalid_csrf = request(path, body={**body, 'csrf': 'invalid'})[0]
                                code, response = request(path, body=body)
                                self.assertEqual(anonymous, 403)
                                self.assertEqual(invalid_csrf, 403)
                                self.assertEqual(code, 400)
                                self.assertEqual(json.loads(response)['error'], 'Camera does not have PTZ enabled')
                                results.append({'path': path, 'anonymousStatus': anonymous,
                                                'invalidCsrfStatus': invalid_csrf, 'disabledStatus': code})
                            self.assertEqual(request('/ptz/999/left', body={'csrf': csrf, 'speed': 2})[0], 403)
                            with (artifacts / f'full-server-ptz-guard-run-{run}.json').open('x', encoding='utf8') as output:
                                json.dump({'nativeBuildId': identity, 'passed': True, 'routes': results,
                                           'scope': 'Actual configured disabled-camera routes and auth; no cached or connected PTZ transport'}, output, indent=2)
                        if raw_processing:
                            self.assertEqual(request('/debug/health?workSamples=bad', False)[0], 403)
                            self.assertEqual(request('/debug/health?workSamples=bad')[0], 400)
                            code, ordinary = request('/debug/health?workSamples=0')
                            self.assertEqual(code, 200)
                            ordinary = json.loads(ordinary)
                            self.assertFalse(ordinary['coverage']['rawWorkSamplesRequested'])
                            self.assertTrue(all('rawSamplesNS' not in camera['processing']['workLatency']
                                                for camera in ordinary['cameras']))
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
                                    'id': 'fixture_mse', 'lastError': {
                                        'type': 'wsError', 'wsReadyState': 3, 'intentionalClose': True,
                                        'generation': 4, 'message': 'https://secret@camera',
                                        'drift': {'paused': True, 'lagMs': 99000, 'password': 'secret'}},
                                    'lastDriftRestart': {
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
                            if len(export['history']) == (3 if multi_client_history and run > 0 else 1) and export['durableThroughSequence'] >= 1:
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
                        error = export['history'][0]['snapshot']['players'][0]['lastError']
                        self.assertEqual(error['type'], 'wsError')
                        self.assertEqual(error['wsReadyState'], 3)
                        self.assertTrue(error['intentionalClose'])
                        self.assertEqual(error['generation'], 4)
                        self.assertEqual(error['drift']['lagMs'], 99000)
                        self.assertNotIn('secret', json.dumps(error))
                        if multi_client_history:
                            report = check_multi_client_history(request, principals, data / 'server.db', restarted=run > 0)
                            with (artifacts / f'full-server-multi-client-history-run-{run}.json').open('x', encoding='utf8') as output:
                                json.dump({'nativeBuildId': identity, **report}, output, indent=2)
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
                                failures = replay_sender_failures(senders, root, inputs, urls)
                                if failures:
                                    server_logs = []
                                    for log_path in sorted((data / 'logs').glob('*.log'))[-4:]:
                                        with log_path.open('rb') as source:
                                            source.seek(0, 2)
                                            source.seek(max(0, source.tell() - 16384))
                                            tail = source.read(16384).decode('utf-8', errors='replace')
                                        for private in [token, csrf, str(root), *map(str, inputs), *urls]:
                                            tail = tail.replace(private, '[fixture]')
                                        server_logs.append(tail)
                                    report = artifacts / ('replay-sender-failed-' + secrets.token_hex(8) + '.json')
                                    with report.open('x', encoding='utf-8') as destination:
                                        json.dump({'nativeBuildId': identity, 'senders': failures, 'health': health,
                                                   'serverLogTails': server_logs}, destination, indent=2)
                                    print('replaySenderFailure=' + str(report), flush=True)
                                self.assertFalse(failures, 'replay sender exited; bounded evidence preserved')
                                if time.monotonic() >= deadline:
                                    (artifacts / 'full-server-replay-readiness-failure.json').write_text(
                                        json.dumps(health, indent=2), encoding='utf-8')
                                    for source in (data / 'logs').glob('*.log'):
                                        shutil.copyfile(source, artifacts / 'full-server-replay-readiness-failure.log')
                                    for index in range(min(2, len(senders))):
                                        shutil.copyfile(root / f'replay-{index}.log',
                                                        artifacts / f'full-server-replay-sender-{index}.log')
                                self.assertLess(time.monotonic(), deadline, 'camera ingress readiness timeout')
                                time.sleep(0.25)
                            initial = {camera['cameraId']: camera['streams'][0]['acceptedVideoPackets'] for camera in cameras}
                            if profiles is not None:
                                validate_live_profiles(cameras, profiles)
                            initial_processing = {camera['cameraId']: camera['processing']['completedFrames'] for camera in cameras}
                            if active_recording:
                                # Normal authenticated recording path, not manufactured DB rows.
                                # These are explicit manual clips, not proof of motion activation.
                                for camera_id in initial:
                                    self.assertEqual(request(f'/camera/record/{camera_id}',
                                        body={'csrf': csrf, 'record': True})[0], 200)
                            browser_layout_evidence = None
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
                                    if browser_gate_viewport is not None:
                                        proof_path = artifacts / 'full-server-browser-gate-proof.json'
                                        try:
                                            if proof_path.stat().st_size > 65536:
                                                raise ValueError('Browser proof exceeds the bounded size')
                                            proof = json.loads(proof_path.read_text(encoding='utf8'))
                                            proof_validated_unix_ms = int(time.time() * 1000)
                                            validate_browser_layout_proof(proof, profiles, browser_gate_viewport,
                                                                          proof_validated_unix_ms)
                                        except (OSError, ValueError, KeyError, TypeError, AttributeError):
                                            self.assertLess(time.monotonic(), deadline, 'Browser layout proof timeout')
                                            time.sleep(1)
                                            continue
                                    if browser_playback_ready(json.loads(history), initial.keys(),
                                            health['server']['webBuildHash'], int(time.time() * 1000),
                                            os.environ.get('WITNESS_TEST_PRESENTATION_MONITOR') == '1'):
                                        print('browserPlaybackReady=true', flush=True)
                                        if browser_gate_viewport is not None:
                                            browser_layout_evidence = {'proof': proof,
                                                'validatedUnixMs': proof_validated_unix_ms}
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
                            work_collector = None
                            if raw_processing:
                                code, initial_work = request('/debug/health')
                                self.assertEqual(code, 200)
                                work_collector = ProcessingSampleCollector(json.loads(initial_work))
                            measurement_started_unix_ms = int(time.time() * 1000)
                            measurement_started_monotonic = time.monotonic()
                            until = measurement_started_monotonic + duration
                            while time.monotonic() < until:
                                health, http_timing = sample_health(request, raw_processing)
                                if work_collector is not None:
                                    work_collector.add(health)
                                    for camera in health['cameras']:
                                        camera['processing']['workLatency'].pop('rawSamplesNS', None)
                                samples.append({'receivedUnixMs': int(time.time() * 1000),
                                    'elapsedSeconds': duration - max(0, until - time.monotonic()),
                                    **http_timing,
                                    'server': health['server'], 'cameras': health['cameras']})
                                self.assertTrue(all(sender.poll() is None for sender in senders), 'replay sender exited')
                                time.sleep(1)
                            full_phase_work = []
                            if profiles is not None:
                                validate_live_profiles(health['cameras'], profiles)
                            if work_collector is not None:
                                code, final_work = request('/debug/health')
                                self.assertEqual(code, 200)
                                work_collector.add(json.loads(final_work))
                                full_phase_work = work_collector.report()
                            measurement_completed_unix_ms = int(time.time() * 1000)
                            measurement_elapsed_seconds = time.monotonic() - measurement_started_monotonic
                            if browser_hold:
                                code, history = request('/debug/client-health')
                                self.assertEqual(code, 200)
                                (artifacts / 'full-server-browser-history.json').write_bytes(history)
                            coverage = sample_coverage(samples, duration)
                            for name, expected in expected_server_traces.items():
                                if expected is None:
                                    continue
                                trace = health['server'][name]
                                self.assertIs(trace['tracingEnabled'], expected == '1')
                                if expected == '0':
                                    self.assertEqual(trace['unavailableReason'], 'disabledByConfiguration')
                                    self.assertIsNone(trace['completed' if name == 'http' else 'queries'])
                                    if name == 'http':
                                        self.assertTrue(all(value is None for value in trace['handlerDuration'].values()))
                                        self.assertEqual(trace['activeRequests'], [])
                                        self.assertEqual(trace['recentSlowRequests'], [])
                                    else:
                                        self.assertIsNone(trace['meanExecuteMs'])
                                        self.assertEqual(trace['recentSlowQueries'], [])
                                else:
                                    self.assertGreater(trace['completed' if name == 'http' else 'queries'], 0)
                            if active_recording:
                                for camera_id in initial:
                                    self.assertEqual(request(f'/camera/record/{camera_id}',
                                        body={'csrf': csrf, 'record': False})[0], 200)
                            for camera in health['cameras']:
                                if expected_media_trace is not None:
                                    self.assertIs(camera['streams'][0]['mediaTraceEnabled'],
                                                  expected_media_trace == '1',
                                                  'health coverage does not match requested media-trace mode')
                                    if expected_media_trace == '0':
                                        self.assertEqual(camera['streams'][0]['recentMediaEvents'], [])
                                if expected_mux_trace is not None:
                                    self.assertIs(camera['streams'][0]['muxTraceEnabled'],
                                                  expected_mux_trace == '1',
                                                  'health coverage does not match requested mux-trace mode')
                                if expected_packet_trace is not None:
                                    self.assertIs(camera['streams'][0]['packetTraceEnabled'],
                                                  expected_packet_trace == '1',
                                                  'health coverage does not match requested packet-trace mode')
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
                            (artifacts / f'full-server-replay-run-{run}.json').write_text(
                                json.dumps({'nativeBuildId': identity, 'cameraCount': len(inputs),
                                    'limits': 'Paced loopback packet-copy replay; browser attachment is externally verified; no baseline phase; not the full load gate',
                                    'coverage': coverage,
                                    'measurementWindow': {
                                        'startedUnixMs': measurement_started_unix_ms,
                                        'completedUnixMs': measurement_completed_unix_ms,
                                        'observedDurationSeconds': measurement_elapsed_seconds},
                                    'processingTaskDistributions': full_phase_work,
                                    'replayProfiles': profiles,
                                    'browserReadyBeforePhase': bool(browser_hold),
                                    'browserLayoutEvidence': browser_layout_evidence,
                                    'workload': {'objectDetection': active_detection, 'provider': 'cpu' if active_detection else None,
                                                 'maximumMotionInferenceFps': 1 if active_detection else None,
                                                 'recording': 'continuous DVR plus authenticated manual clips' if active_recording else 'disabled',
                                                 'strictRecordingInspection': verify_outputs,
                                                 'motionThreshold': 0.01 if active_detection else 0.99},
                                    'samples': samples}, indent=2), encoding='utf-8')
                            if work_collector is not None:
                                raw_path = artifacts / f'full-server-processing-samples-run-{run}.json.gz'
                                with gzip.open(raw_path, 'wt', encoding='utf-8') as raw_file:
                                    json.dump(work_collector.raw_evidence(), raw_file, separators=(',', ':'))
                                self.assertTrue(full_phase_work and all(row['complete'] for row in full_phase_work),
                                                'Per-task phase evidence has gaps/resets/overruns')
                    finally:
                        slow_dump = None
                        def capture_slow_shutdown():
                            nonlocal slow_dump
                            try:
                                candidate = artifacts / f'owned-slow-shutdown-{secrets.token_hex(8)}.dmp'
                                capture_owned_process_dump(process, candidate, include_referenced_memory=True)
                                slow_dump = candidate
                                print(f'Owned slow shutdown dump: {candidate}', flush=True)
                            except (OSError, ValueError):
                                pass  # Observation must not replace the shutdown result.
                        try:
                            stop_console_process(process, capture_slow_shutdown)
                        except BaseException:
                            # Preserve a failing lifecycle before private fixture cleanup.
                            log.flush()
                            shutil.copyfile(root / f'run-{run}.log', artifacts / 'full-server-isolation-failed-run.log')
                            for source in (data / 'logs').glob('*.log'):
                                shutil.copyfile(source, artifacts / 'full-server-isolation-failed-server.log')
                            debugger = Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
                            if process.poll() is None:
                                try:
                                    dump = artifacts / f'owned-shutdown-{secrets.token_hex(8)}.dmp'
                                    capture_owned_process_dump(process, dump, include_referenced_memory=True)
                                    print(f'Owned shutdown dump: {dump}', flush=True)
                                    if debugger.is_file():
                                        subprocess.run([str(debugger), '-z', str(dump),
                                            '-y', str(server.parent), '-logo', str(dump.with_suffix('.stacks.log')),
                                            '-c', '~* kb; q'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL, timeout=20,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                                except (OSError, subprocess.TimeoutExpired):
                                    pass  # Diagnostics never prevent test-owned process cleanup.
                            raise
                        finally:
                            if process.poll() is None:
                                process.kill()
                                process.wait(timeout=5)
                            if slow_dump is not None:
                                debugger = Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
                                if debugger.is_file():
                                    try:
                                        subprocess.run([str(debugger), '-z', str(slow_dump),
                                            '-y', str(server.parent), '-logo', str(slow_dump.with_suffix('.stacks.log')),
                                            '-c', '~* kb; q'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL, timeout=20,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                                    except (OSError, subprocess.TimeoutExpired):
                                        pass
                    if process.returncode != 0:
                        log.flush()
                        shutil.copyfile(root / f'run-{run}.log', artifacts / 'full-server-isolation-failed-run.log')
                        for source in (data / 'logs').glob('*.log'):
                            shutil.copyfile(source, artifacts / 'full-server-isolation-failed-server.log')
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
                            progress_path = (artifacts / f'full-server-recording-progress-run-{run}.json'
                                if os.environ.get('WITNESS_TEST_LONG_RECORDING_INSPECTION') == '1' else None)
                            decoded = verify_recordings(db, cache, previous_clip_uid, previous_segment_uid,
                                                        progress_path, artifacts,
                                                        frame_decoder_threads) if verify_outputs else []
                        (artifacts / f'full-server-workload-run-{run}.json').write_text(
                            json.dumps({'completedInferenceWindows': evidence, 'clipRows': clips,
                                        'continuousSegmentRows': segments,
                                        'recordingInspection': decoded,
                                        'limits': 'Rows alone prove creation; optional inspection checks strict decode and packet/frame timestamps, not perceptual smoothness; clips are manually triggered, not proof of MV activation; inferred frames include 30-second no-motion baselines'}, indent=2), encoding='utf-8')
                        if active_detection:
                            self.assertEqual({row['cameraId'] for row in evidence}, set(range(1, len(inputs) + 1)),
                                             'Every camera must complete real inference, not just load a model')
                        self.assertEqual({row['cameraId'] for row in clips}, set(range(1, len(inputs) + 1)),
                                         'Every camera must create a new recorded clip')
                        self.assertTrue(recording_metadata_finalized(clips, (browser_hold or replay_seconds) + 360),
                                        'Every recorded clip must persist its final duration before shutdown')
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
