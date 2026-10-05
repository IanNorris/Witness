"""Pure evidence-validator tests: no server, model, cameras or credentials."""
import unittest
import sqlite3
import json
import tempfile
from pathlib import Path
from unittest.mock import patch
from unittest.mock import Mock
from copy import deepcopy
from check_preview_decode import RecordingInspectionError

from test_full_server_isolation import browser_playback_ready, inference_evidence, recording_evidence, replay_sender_failures, sample_coverage, verify_recordings


class WorkloadEvidenceTests(unittest.TestCase):
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
            finally:
                db.close()

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
            db.execute('CREATE TABLE Clip(ClipUID INTEGER,Camera INTEGER)')
            db.execute('CREATE TABLE ContinuousSegment(SegmentUID INTEGER,CameraUID INTEGER)')
            db.executemany('INSERT INTO Clip VALUES(?,?)', [(1, 1), (2, 1), (3, 2)])
            db.executemany('INSERT INTO ContinuousSegment VALUES(?,?)', [(1, 1), (2, 2)])
            self.assertEqual(recording_evidence(db, 3, 2), ([], []))
            self.assertEqual(recording_evidence(db, 2, 1),
                             ([{'cameraId': 2, 'clips': 1}], [{'cameraId': 2, 'segments': 1}]))
        finally:
            db.close()

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
