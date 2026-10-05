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

    def test_window_scopes_server_receipts_without_guessing_legacy_times(self):
        replay, row = self.fixture()
        replay['measurementWindow'] = {'startedUnixMs': 1500, 'completedUnixMs': 2500}
        current = copy.deepcopy(row)
        current['receivedUnixMs'] = 2000
        summary = summarize(replay, {'history': [row, current]})
        self.assertEqual(summary['browserHistoryScope'], 'serverReceiptWithinMeasurementWindow')
        self.assertEqual(summary['browserSessions'][0]['retainedRows'], 1)
        del replay['measurementWindow']
        summary = summarize(replay, {'history': [row, current]})
        self.assertEqual(summary['browserHistoryScope'], 'legacyUnanchoredHistory')
        self.assertEqual(summary['browserSessions'][0]['retainedRows'], 2)

    def test_invalid_window_is_not_silently_ignored(self):
        replay, _ = self.fixture()
        for window in ({}, {'startedUnixMs': True, 'completedUnixMs': 2000},
                       {'startedUnixMs': 2000, 'completedUnixMs': 1000}):
            replay['measurementWindow'] = window
            with self.assertRaises(ValueError):
                summarize(replay, {})

    def test_retained_tail_cannot_look_like_full_phase_coverage(self):
        replay, row = self.fixture()
        replay['measurementWindow'] = {'startedUnixMs': 1000, 'completedUnixMs': 1801000}
        tail = copy.deepcopy(row)
        tail['receivedUnixMs'] = 1201000
        last = copy.deepcopy(row)
        last['receivedUnixMs'] = 1781000
        session = summarize(replay, {'history': [tail, last]})['browserSessions'][0]
        boundaries = session['measurementBoundaryObservations']
        self.assertEqual(boundaries['startOffsetSeconds'], 1200)
        self.assertEqual(boundaries['endGapSeconds'], 20)
        self.assertFalse(boundaries['startObservedWithin30Seconds'])
        self.assertTrue(boundaries['endObservedWithin30Seconds'])
        self.assertEqual(session['maximumReceiptGapSeconds'], 580)
        self.assertEqual(session['firstReceivedUnixMs'], 1201000)
        self.assertEqual(session['lastReceivedUnixMs'], 1781000)

    def test_single_row_and_legacy_history_have_no_invented_gap_or_boundaries(self):
        replay, row = self.fixture()
        session = summarize(replay, {'history': [row]})['browserSessions'][0]
        self.assertIsNone(session['maximumReceiptGapSeconds'])
        self.assertIsNone(session['measurementBoundaryObservations'])


if __name__ == '__main__':
    unittest.main()
