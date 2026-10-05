import copy
import unittest
from summarize_replay_load import quantile, summarize


class ReplaySummaryTests(unittest.TestCase):
    def fixture(self):
        player = {'id': '1_mse', 'restartCount': 0, 'stallCount': 0, 'errorCount': 0,
                  'totalVideoFrames': 10, 'droppedVideoFrames': 0, 'readyState': 4}
        row = {'receivedUnixMs': 1000, 'userUid': 1, 'username': 'fixture-admin',
               'browserId': 'b', 'sessionId': 's', 'snapshot': {'buildHash': 'web',
               'path': '/', 'visibilityState': 'visible', 'players': [player]}}
        sample = {'httpMs': 5, 'server': {'nativeBuildHash': 'native', 'webBuildHash': 'web',
                  'host': {'processCpuPercent': 10, 'cpuPercent': 20}},
                  'cameras': [{'cameraId': 1, 'processing': {'completedFrames': 5,
                  'workLatency': {'p95Ms': 2, 'scope': 'filterOnly'}}}]}
        return {'nativeBuildId': 'native', 'samples': [sample, copy.deepcopy(sample)]}, row

    def test_nonfinite_and_missing_are_not_zero(self):
        self.assertIsNone(quantile([None, float('nan'), True], .95))
        self.assertEqual(quantile([1, 3, 2], .95), 3)

    def test_synthetic_other_build_and_hidden_rows_are_excluded(self):
        replay, row = self.fixture()
        invalid = []
        for field, value in [('buildHash', 'other'), ('visibilityState', 'hidden'), ('players', [])]:
            changed = copy.deepcopy(row)
            changed['snapshot'][field] = value
            invalid.append(changed)
        duplicate = copy.deepcopy(row)
        duplicate['snapshot']['players'] *= 2
        summary = summarize(replay, {'history': invalid + [duplicate, row]})
        self.assertEqual(summary['browserSessions'][0]['retainedRows'], 1)
        self.assertEqual(len(summary['limits']), 6)

    def test_session_replacement_not_combined_and_counter_reset_flagged(self):
        replay, row = self.fixture()
        reset = copy.deepcopy(row)
        reset['receivedUnixMs'] = 2000
        reset['snapshot']['players'][0]['totalVideoFrames'] = 3
        other = copy.deepcopy(row)
        other['sessionId'] = 'new'
        recovered = copy.deepcopy(row)
        recovered['receivedUnixMs'] = 3000
        recovered['snapshot']['players'][0]['totalVideoFrames'] = 20
        summary = summarize(replay, {'history': [row, reset, recovered, other]})
        self.assertEqual(len(summary['browserSessions']), 2)
        self.assertTrue(summary['browserSessions'][0]['players'][0]['counterResetObserved'])

    def test_mixed_build_and_camera_sets_rejected(self):
        replay, _ = self.fixture()
        replay['samples'][1]['server']['nativeBuildHash'] = 'different'
        with self.assertRaises(ValueError):
            summarize(replay, {})
        replay, _ = self.fixture()
        replay['samples'][1]['cameras'] = []
        with self.assertRaises(ValueError):
            summarize(replay, {})


if __name__ == '__main__':
    unittest.main()
