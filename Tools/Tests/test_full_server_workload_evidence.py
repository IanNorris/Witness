"""Pure evidence-validator tests: no server, model, cameras or credentials."""
import unittest
import sqlite3

from test_full_server_isolation import inference_evidence, recording_evidence, sample_coverage


class WorkloadEvidenceTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
