"""Pure evidence-validator tests: no server, model, cameras or credentials."""
import unittest
import sqlite3
import json
import tempfile
from pathlib import Path
from unittest.mock import patch
from unittest.mock import Mock, ANY
from copy import deepcopy
from contextlib import closing
from check_preview_decode import RecordingInspectionError

from test_full_server_isolation import artifact_directory, browser_playback_ready, inference_evidence, recording_evidence, recording_metadata_finalized, replay_sender_failures, sample_coverage, verify_recordings, replace_progress_snapshot


class WorkloadEvidenceTests(unittest.TestCase):
    def test_progress_publish_retries_only_bounded_windows_sharing_conflicts(self):
        for code in (5, 32):
            conflict = PermissionError('reader still has the old snapshot open')
            conflict.winerror = code
            temporary, target = Mock(), Mock()
            temporary.replace.side_effect = [conflict, conflict, None]
            with patch('test_full_server_isolation.os.name', 'nt'), \
                    patch('test_full_server_isolation.time.sleep') as sleep:
                replace_progress_snapshot(temporary, target)
            self.assertEqual(temporary.replace.call_count, 3)
            self.assertEqual(sleep.call_count, 2)
            for call in sleep.call_args_list:
                self.assertEqual(call.args, (0.05,))
        temporary.replace.side_effect = conflict
        temporary.replace.reset_mock()
        with patch('test_full_server_isolation.os.name', 'nt'), \
                patch('test_full_server_isolation.time.sleep') as sleep, self.assertRaises(PermissionError):
            replace_progress_snapshot(temporary, target)
        self.assertEqual(temporary.replace.call_count, 20)
        self.assertEqual(sleep.call_count, 19)
        for platform, error in (('posix', conflict), ('nt', OSError('disk IO failure'))):
            temporary.replace.side_effect = error
            temporary.replace.reset_mock()
            with patch('test_full_server_isolation.os.name', platform), \
                    patch('test_full_server_isolation.time.sleep') as sleep, self.assertRaises(type(error)):
                replace_progress_snapshot(temporary, target)
            self.assertEqual(temporary.replace.call_count, 1)
            sleep.assert_not_called()

    def test_resume_keeps_only_matching_passed_prefix_and_inspects_remaining(self):
        with tempfile.TemporaryDirectory() as directory, closing(sqlite3.connect(':memory:')) as db:
            cache = Path(directory)
            for camera in (1, 2):
                (cache / f'{camera}_Manual_100.mp4').touch()
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
            db.executemany('INSERT INTO Clip VALUES(?,?,100,0)', [(1, 1), (2, 2)])
            first = dict(kind='clip', cameraId=1, recordingId=1, passed=True)
            with patch.dict('os.environ', {'WITNESS_TEST_LONG_RECORDING_INSPECTION': '0',
                                          'WITNESS_TEST_VERIFY_AUDIO_RECORDINGS': '0'}), \
                    patch('test_full_server_isolation.inspect_recording', return_value={'passed': True}) as inspect:
                reports = verify_recordings(db, cache, 0, 0, completed_reports=[first])
                self.assertEqual(reports[0], first)
                self.assertEqual(len(reports), 2)
                inspect.assert_called_once_with((cache / '2_Manual_100.mp4').resolve())
                inspect.reset_mock()
                for invalid in ([{**first, 'passed': False}], [{**first, 'cameraId': 2}],
                                [first, first, first]):
                    with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                        verify_recordings(db, cache, 0, 0, completed_reports=invalid)
                inspect.assert_not_called()
            with patch.dict('os.environ', {'WITNESS_TEST_VERIFY_AUDIO_RECORDINGS': '1'}), \
                    self.assertRaisesRegex(ValueError, 'audio'):
                verify_recordings(db, cache, 0, 0, completed_reports=[first], expected_audio_cameras=[1])
            with patch.dict('os.environ', {'WITNESS_TEST_LONG_RECORDING_INSPECTION': '1',
                                          'WITNESS_TEST_VERIFY_AUDIO_RECORDINGS': '0'}), \
                    self.assertRaisesRegex(ValueError, 'settings'):
                verify_recordings(db, cache, 0, 0, frame_decoder_threads=4, completed_reports=[
                    dict(first, inspectionMode='boundedStreamingFullRecording', frameDecoderThreads=1)])

    def test_required_audio_cannot_pass_when_track_is_missing(self):
        with tempfile.TemporaryDirectory() as directory, closing(sqlite3.connect(':memory:')) as db:
            cache = Path(directory)
            (cache / '1_Manual_100.mp4').touch()
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
            db.execute('INSERT INTO Clip VALUES(1,1,100,0)')
            for present, audio_passed, expected in ((False, True, False), (True, False, False), (True, True, True)):
                with self.subTest(present=present, audio_passed=audio_passed), \
                     patch.dict('os.environ', {'WITNESS_TEST_VERIFY_AUDIO_RECORDINGS': '1'}), \
                     patch('test_full_server_isolation.inspect_recording', return_value={'passed': True}), \
                     patch('builtins.print'), patch('test_full_server_isolation.inspect_audio', return_value={
                         'audioPresent': present, 'passed': audio_passed}):
                    report = verify_recordings(db, cache, 0, 0, failure_root=cache, expected_audio_cameras=[1])[0]
                self.assertEqual(report['passed'], expected)
                self.assertTrue(report['audio']['required'])

    def test_long_inspection_receives_explicit_frame_thread_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            recording = cache / '1_Manual_100.mp4'
            recording.touch()
            db = sqlite3.connect(':memory:')
            try:
                db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
                db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
                db.execute('INSERT INTO Clip VALUES(1,1,100,0)')
                with patch.dict('os.environ', {'WITNESS_TEST_LONG_RECORDING_INSPECTION': '1'}), \
                     patch('test_full_server_isolation.inspect_long', return_value={
                         'passed': True, 'frameDecoderThreads': 4}) as inspect:
                    reports = verify_recordings(db, cache, 0, 0, frame_decoder_threads=4)
                inspect.assert_called_once_with(recording.resolve(), frame_decoder_threads=4,
                                                progress_callback=ANY)
                self.assertTrue(callable(inspect.call_args.kwargs['progress_callback']))
                self.assertEqual(reports[0]['frameDecoderThreads'], 4)
                self.assertEqual(reports[0]['cameraId'], 1)
            finally:
                db.close()

    def test_active_inspection_stage_is_incomplete_and_final_result_clears_it(self):
        with tempfile.TemporaryDirectory() as directory, closing(sqlite3.connect(':memory:')) as db:
            cache = Path(directory)
            (cache / '1_Manual_100.mp4').touch()
            progress = cache / 'progress.json'
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
            db.execute('INSERT INTO Clip VALUES(1,1,100,0)')
            stages = []
            def inspect(path, frame_decoder_threads, progress_callback):
                for phase in ('videoStrictDecode', 'videoPackets', 'videoFrames'):
                    progress_callback(phase, None)
                    row = json.loads(progress.read_text(encoding='utf8'))
                    self.assertFalse(row['inspectionComplete'])
                    self.assertFalse(row['passed'])
                    self.assertEqual(row['completedOutputs'], 0)
                    current = row['currentInspection']
                    self.assertEqual((current['kind'], current['cameraId'], current['recordingId']), ('clip', 1, 1))
                    self.assertEqual(set(current), {'kind', 'cameraId', 'recordingId', 'startedUnixMs',
                                                   'stage', 'streamIndex', 'stageStartedUnixMs'})
                    self.assertGreaterEqual(current['stageStartedUnixMs'], current['startedUnixMs'])
                    stages.append(current['stage'])
                return {'passed': True}
            with patch.dict('os.environ', {'WITNESS_TEST_LONG_RECORDING_INSPECTION': '1',
                                          'WITNESS_TEST_VERIFY_AUDIO_RECORDINGS': '0'}), \
                 patch('test_full_server_isolation.inspect_long', side_effect=inspect):
                verify_recordings(db, cache, 0, 0, progress_path=progress)
            self.assertEqual(stages, ['videoStrictDecode', 'videoPackets', 'videoFrames'])
            final = json.loads(progress.read_text(encoding='utf8'))
            self.assertTrue(final['inspectionComplete'])
            self.assertTrue(final['passed'])
            self.assertIsNone(final['currentInspection'])
            self.assertEqual(final['completedOutputs'], 1)

    def test_artifact_namespaces_are_exclusive_and_leave_other_runs_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            shared = artifact_directory(repository)
            sentinel = shared / 'full-server-workload-run-0.json'
            sentinel.write_text('other live run')
            first = artifact_directory(repository, 'gap-stream-check')
            second = artifact_directory(repository, 'another-run')
            self.assertNotEqual(first, second)
            self.assertEqual(first.parent, shared)
            self.assertTrue(first.name.startswith('run-'))
            with self.assertRaises(FileExistsError):
                artifact_directory(repository, 'gap-stream-check')
            for name in ('', '../escape', 'a/b', 'A', 'a'*65, 'a.b', 'a\\b', 1):
                with self.assertRaises(ValueError):
                    artifact_directory(repository, name)
            self.assertEqual(sentinel.read_text(), 'other live run')

    def test_artifact_root_links_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch('pathlib.Path.is_junction', return_value=True):
                with self.assertRaises(ValueError):
                    artifact_directory(Path(directory), 'linked-root')

    def test_partial_recording_progress_survives_later_failure_without_private_paths(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            cache = root / 'cache'; cache.mkdir()
            (cache / '1_Manual_100.mp4').touch()
            (cache / '2_Manual_100.mp4').touch()
            progress = root / 'progress.json'
            db = sqlite3.connect(':memory:')
            try:
                db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
                db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
                db.executemany('INSERT INTO Clip VALUES(?,?,?,?)', [(1,1,100,0),(2,2,100,0)])
                for second in (RecordingInspectionError('frames', 7, 'failure'), {'passed': False}, {'passed': True}):
                    with self.subTest(second=type(second).__name__), \
                         patch('test_full_server_isolation.inspect_recording', side_effect=[
                             {'passed': True, 'file': str(cache / 'private.mp4')}, second]), \
                         patch('test_full_server_isolation.RECORDING_FAILURE_ROOT', root), patch('builtins.print'):
                        if isinstance(second, Exception):
                            with self.assertRaises(RecordingInspectionError):
                                verify_recordings(db, cache, 0, 0, progress)
                            expected_count, complete = 1, False
                        else:
                            verify_recordings(db, cache, 0, 0, progress)
                            expected_count, complete = 2, True
                    report = json.loads(progress.read_text())
                    self.assertEqual(report['completedOutputs'], expected_count)
                    self.assertEqual(report['expectedOutputs'], 2)
                    self.assertEqual(report['inspectionComplete'], complete)
                    expected_pass = isinstance(second, dict) and complete and second.get('passed', False)
                    self.assertEqual(report['passed'], expected_pass)
                    self.assertNotIn(str(cache), progress.read_text())
                    self.assertFalse(progress.with_suffix('.tmp').exists())
            finally:
                db.close()

    def test_sender_failure_evidence_is_bounded_and_omits_private_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / 'private-fixture.mp4'
            url = 'http://127.0.0.1:1234/fixture'
            (root / 'replay-0.log').write_text('discarded-prefix' * 2000 + str(root) + ' ' + str(fixture) + ' ' + url,
                                             encoding='utf-8')
            running = Mock()
            running.poll.return_value = None
            failed = Mock()
            failed.poll.return_value = 7
            rows = replay_sender_failures([failed, running], root, [fixture, fixture], [url, url])
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['cameraId'], 1)
            self.assertEqual(rows[0]['returnCode'], 7)
            self.assertLessEqual(len(rows[0]['diagnostics']), 16384)
            for private in (str(root), str(fixture), url):
                self.assertNotIn(private, rows[0]['diagnostics'])

    def test_failed_inspection_preserves_only_owned_recording_and_still_fails(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / 'cache'
            evidence = Path(root) / 'evidence'
            cache.mkdir()
            evidence.mkdir()
            recording = cache / 'fixture.mp4'
            recording.write_bytes(b'fixture recording bytes')
            db = sqlite3.connect(':memory:')
            try:
                db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
                db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
                db.execute('INSERT INTO ContinuousSegment VALUES(1,8,?)', (str(recording),))
                failure = RecordingInspectionError('packets', 7, 'probe failure')
                with patch('test_full_server_isolation.RECORDING_FAILURE_ROOT', evidence), \
                     patch('test_full_server_isolation.inspect_recording', side_effect=failure), \
                     patch('builtins.print'):
                    with self.assertRaises(RecordingInspectionError) as caught:
                        verify_recordings(db, cache, 0, 0)
                self.assertIs(caught.exception, failure)
                reports = list(evidence.glob('*.json'))
                self.assertEqual(len(reports), 1)
                details = json.loads(reports[0].read_text(encoding='utf-8'))
                self.assertFalse(details['passed'])
                self.assertEqual(details['cameraId'], 8)
                self.assertEqual(details['returnCode'], 7)
                self.assertEqual(details['diagnostics'], 'probe failure')
                self.assertEqual((evidence / details['savedRecording']).read_bytes(), recording.read_bytes())
                self.assertNotIn(str(cache), reports[0].read_text(encoding='utf-8'))
                self.assertEqual(len(list(evidence.iterdir())), 2)
                isolated = evidence / 'isolated'; isolated.mkdir()
                with patch('test_full_server_isolation.RECORDING_FAILURE_ROOT', Path(root) / 'must-not-create'), \
                     patch('test_full_server_isolation.inspect_recording', side_effect=failure), \
                     patch('builtins.print'):
                    with self.assertRaises(RecordingInspectionError):
                        verify_recordings(db, cache, 0, 0, failure_root=isolated)
                self.assertEqual(len(list(isolated.glob('*.json'))), 1)
                self.assertEqual(len(list(isolated.glob('*.mp4'))), 1)
                self.assertFalse((Path(root) / 'must-not-create').exists())
            finally:
                db.close()

    def test_negative_integrity_result_preserves_repro_without_stopping_full_audit(self):
        with tempfile.TemporaryDirectory() as directory, closing(sqlite3.connect(':memory:')) as db:
            root = Path(directory)
            cache = root / 'cache'; cache.mkdir()
            evidence = root / 'evidence'; evidence.mkdir()
            recording = cache / 'fixture.mp4'
            recording.write_bytes(b'only synthetic fixture bytes')
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
            db.execute('INSERT INTO ContinuousSegment VALUES(1,8,?)', (str(recording),))
            progress = root / 'progress.json'
            result = {'file': str(recording), 'passed': False, 'strictDecodeExit': 1,
                      'packets': {'duplicates': 1}}
            with patch('test_full_server_isolation.inspect_recording', return_value=result), \
                 patch('builtins.print'):
                reports = verify_recordings(db, cache, 0, 0, progress, evidence)
            self.assertFalse(reports[0]['passed'])
            status = json.loads(progress.read_text())
            self.assertTrue(status['inspectionComplete'])
            self.assertFalse(status['passed'])
            self.assertEqual(status['completedOutputs'], 1)
            details = json.loads(next(evidence.glob('*.json')).read_text())
            self.assertEqual(details['stage'], 'reportedIntegrityFailure')
            self.assertEqual(details['returnCode'], 1)
            self.assertEqual(details['inspectionResult']['packets']['duplicates'], 1)
            self.assertNotIn('file', details['inspectionResult'])
            self.assertNotIn(str(cache), json.dumps(details))
            self.assertEqual((evidence / details['savedRecording']).read_bytes(), recording.read_bytes())
            self.assertEqual(len(list(evidence.iterdir())), 2)

    def test_browser_phase_requires_fresh_matching_playing_players(self):
        player = {'id': '1_mse', 'readyState': 4, 'wsReadyState': 1, 'lastEventType': 'media:playing',
                  'totalVideoFrames': 10, 'errorCount': 0, 'hasInitialBuffer': True,
                  'waitingForKeyframe': False, 'awaitingInit': False,
                  'driftSamples': [{'visibility': 'visible', 'paused': False, 'seeking': False,
                                    'playbackAdvanceMs': 2000, 'sampleIntervalMs': 2000}]}
        healthy = {'history': [{'userUid': 1, 'username': 'fixture-admin', 'receivedUnixMs': 100000,
                              'snapshot': {'buildHash': 'test-build', 'path': '/',
                                           'visibilityState': 'visible', 'players': [player]}}]}
        self.assertTrue(browser_playback_ready(healthy, [1], 'test-build', 100500))
        for field, value in [('players', []), ('buildHash', 'old-build'), ('path', '/health-monitor'),
                             ('visibilityState', 'hidden'), ('visibilityState', None)]:
            export = deepcopy(healthy)
            export['history'][0]['snapshot'][field] = value
            self.assertFalse(browser_playback_ready(export, [1], 'test-build', 100500))
        for field, value in [('readyState', 2), ('wsReadyState', 0), ('lastEventType', 'media:waiting'),
                             ('totalVideoFrames', 0), ('errorCount', 1), ('id', '2_mse'), ('awaitingInit', True)]:
            export = deepcopy(healthy)
            export['history'][0]['snapshot']['players'][0][field] = value
            self.assertFalse(browser_playback_ready(export, [1], 'test-build', 100500))
        self.assertFalse(browser_playback_ready(healthy, [1, 2], 'test-build', 100500))
        self.assertFalse(browser_playback_ready(healthy, [1], 'test-build', 116000))
        self.assertFalse(browser_playback_ready(healthy, [1], 'test-build', 98000))
        self.assertFalse(browser_playback_ready(healthy, [1], 'test-build', 100500, True))
        presented = deepcopy(healthy)
        presented['history'][0]['snapshot']['players'][0]['driftSamples'][-1].update({
            'presentationVisible': True, 'presentedFrames': 30, 'currentTime': 2,
            'presentedMediaTime': 1.95})
        self.assertTrue(browser_playback_ready(presented, [1], 'test-build', 100500, True))
        for field, value in [('presentationVisible', False), ('presentationVisible', 1),
                             ('presentedFrames', True), ('presentedFrames', 1),
                             ('currentTime', float('nan')), ('presentedMediaTime', float('inf')),
                             ('presentedMediaTime', None), ('presentedMediaTime', 0)]:
            invalid = deepcopy(presented)
            invalid['history'][0]['snapshot']['players'][0]['driftSamples'][-1][field] = value
            self.assertFalse(browser_playback_ready(invalid, [1], 'test-build', 100500, True))
        changing = deepcopy(healthy)
        changing['history'][0]['snapshot']['players'][0]['lastEventType'] = 'media:ratechange'
        self.assertTrue(browser_playback_ready(changing, [1], 'test-build', 100500))
        # Compact browser reports omit seeking, but retain measured progress.
        del changing['history'][0]['snapshot']['players'][0]['driftSamples'][-1]['seeking']
        self.assertTrue(browser_playback_ready(changing, [1], 'test-build', 100500))
        for field, value in [('paused', True), ('seeking', True), ('visibility', 'hidden'),
                             ('playbackAdvanceMs', 0), ('playbackAdvanceMs', float('nan')),
                             ('sampleIntervalMs', 60000)]:
            export = deepcopy(changing)
            export['history'][0]['snapshot']['players'][0]['driftSamples'][-1][field] = value
            self.assertFalse(browser_playback_ready(export, [1], 'test-build', 100500))
        export = deepcopy(changing)
        export['history'][0]['snapshot']['players'][0]['driftSamples'] = []
        self.assertFalse(browser_playback_ready(export, [1], 'test-build', 100500))

    def test_continuous_sampling(self):
        report = sample_coverage([{'elapsedSeconds': value} for value in range(1, 91)], 90)
        self.assertEqual(report['maximumGapSeconds'], 1)

    def test_sleep_partial_and_reordered_runs_rejected(self):
        for times in ([], [1, 2, 80, 90], [1, 2], [80, 90], [1, 3, 2, 90], [-1, 90]):
            with self.subTest(times=times), self.assertRaises(ValueError):
                sample_coverage([{'elapsedSeconds': value} for value in times], 90)

    def test_model_ready_and_zero_inferences_are_not_proof(self):
        self.assertEqual(inference_evidence('ONNX model ready secret-url\n'
            'ONNX Camera 1 stats: 0/0 detected (0%), 0 skipped, 0 baseline-filtered, avg 0ms'), [])

    def test_only_numeric_completed_stats_exported(self):
        rows = inference_evidence('[INFO] ONNX Camera 12 stats: 2/31 detected (6%), '
                                 '4 skipped, 3 baseline-filtered, avg 42ms\nsecret-session-token')
        self.assertEqual(rows, [{'cameraId': 12, 'detections': 2, 'completedInferences': 31,
                                'skipped': 4, 'baselineFiltered': 3, 'meanInferenceMsRounded': 42}])

    def test_previous_lifecycle_recordings_cannot_qualify_new_run(self):
        db = sqlite3.connect(':memory:')
        try:
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Duration INTEGER,ActiveDuration INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER)')
            db.executemany('INSERT INTO Clip VALUES(?,?,?,?)', [(1, 1, 10, 0), (2, 1, 20, 0), (3, 2, 30, 0)])
            db.executemany('INSERT INTO ContinuousSegment VALUES(?,?)', [(1, 1), (2, 2)])
            self.assertEqual(recording_evidence(db, 3, 2), ([], []))
            self.assertEqual(recording_evidence(db, 2, 1),
                             ([{'cameraId': 2, 'clips': 1, 'minimumDurationSeconds': 30,
                                'maximumDurationSeconds': 30, 'invalidMotionDurationCount': 0}],
                              [{'cameraId': 2, 'segments': 1}]))
        finally:
            db.close()

    def test_recording_metadata_requires_every_new_clip_to_be_finalized(self):
        self.assertFalse(recording_metadata_finalized([], 120))
        for duration in (None, 0, -1, True, '20'):
            self.assertFalse(recording_metadata_finalized([
                {'minimumDurationSeconds': 20, 'maximumDurationSeconds': 20, 'invalidMotionDurationCount': 0},
                {'minimumDurationSeconds': duration, 'maximumDurationSeconds': 20, 'invalidMotionDurationCount': 0}], 120))
        self.assertTrue(recording_metadata_finalized([
            {'minimumDurationSeconds': 20, 'maximumDurationSeconds': 20, 'invalidMotionDurationCount': 0}], 120))
        for duration in (None, True, 19, 121, 2**63):
            self.assertFalse(recording_metadata_finalized([
                {'minimumDurationSeconds': 20, 'maximumDurationSeconds': duration, 'invalidMotionDurationCount': 0}], 120))
        with closing(sqlite3.connect(':memory:')) as db:
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Duration INTEGER,ActiveDuration INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER)')
            db.executemany('INSERT INTO Clip VALUES(?,?,?,?)', [(1, 1, 20, 0), (2, 1, 0, 0)])
            self.assertFalse(recording_metadata_finalized(recording_evidence(db, 0, 0)[0], 120))
            db.execute('UPDATE Clip SET Duration=20')
            for active in (None, -1, 21):
                db.execute('UPDATE Clip SET ActiveDuration=? WHERE ClipUID=2', (active,))
                self.assertFalse(recording_metadata_finalized(recording_evidence(db, 0, 0)[0], 120))
            db.execute('UPDATE Clip SET ActiveDuration=10')
            self.assertTrue(recording_metadata_finalized(recording_evidence(db, 0, 0)[0], 120))

    def test_output_verifier_rejects_external_paths_before_decoding(self):
        db = sqlite3.connect(':memory:')
        try:
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER,Timestamp INTEGER,RecordMode INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER,FilePath TEXT)')
            db.execute('INSERT INTO ContinuousSegment VALUES(1,1,?)', (str(Path(__file__).resolve()),))
            with patch('test_full_server_isolation.inspect_recording') as decoder:
                with self.assertRaises(ValueError):
                    verify_recordings(db, Path(__file__).resolve().parents[2] / 'artifacts', 0, 0)
                decoder.assert_not_called()
                decoder.return_value = {'file': 'private-path', 'passed': True}
                reports = verify_recordings(db, Path(__file__).resolve().parent, 0, 0)
                self.assertEqual(reports, [{'passed': True, 'kind': 'continuous', 'cameraId': 1, 'recordingId': 1}])
        finally:
            db.close()


if __name__ == '__main__':
    unittest.main()
