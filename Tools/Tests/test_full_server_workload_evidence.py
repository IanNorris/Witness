"""Pure evidence-validator tests: no server, model, cameras or credentials."""
import unittest
import sqlite3
from pathlib import Path
from unittest.mock import patch
from copy import deepcopy

from test_full_server_isolation import browser_playback_ready, inference_evidence, recording_evidence, sample_coverage, verify_recordings


class WorkloadEvidenceTests(unittest.TestCase):
    def test_browser_phase_requires_fresh_matching_playing_players(self):
        player = {'id': '1_mse', 'readyState': 4, 'wsReadyState': 1, 'lastEventType': 'media:playing',
                  'totalVideoFrames': 10, 'errorCount': 0, 'hasInitialBuffer': True,
                  'waitingForKeyframe': False, 'awaitingInit': False}
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
