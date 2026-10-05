import copy
import unittest
from summarize_replay_load import buffer_position_observations, presentation_observations, quantile, summarize


class ReplaySummaryTests(unittest.TestCase):
    def test_observer_modes_and_main_profiles_require_consistent_observed_evidence(self):
        replay, row = self.fixture()
        for sample in replay['samples']:
            sample['server']['http'] = {'tracingEnabled': False}
            sample['server']['database'] = {'tracingEnabled': False}
            sample['cameras'][0]['streams'] = [{'tier': 'main', 'videoCodec': 'h264',
                'width': 1280, 'height': 720, 'audioCodec': '',
                'packetTraceEnabled': False, 'muxTraceEnabled': False, 'mediaTraceEnabled': False}]
        report = summarize(replay, {'history': [row]})
        self.assertTrue(all(value is False for value in report['observerModes'].values()))
        self.assertEqual(report['mainStreamProfiles'][0]['width'], 1280)
        replay['samples'][1]['cameras'][0]['streams'][0]['width'] = 256
        replay['samples'][1]['server']['http']['tracingEnabled'] = True
        replay['samples'][1]['cameras'][0]['streams'][0].pop('packetTraceEnabled')
        report = summarize(replay, {'history': [row]})
        self.assertIsNone(report['mainStreamProfiles'])
        self.assertIsNone(report['observerModes']['http'])
        self.assertIsNone(report['observerModes']['packet'])

    def test_presentation_does_not_bridge_missing_or_conflicting_evidence(self):
        def sample(t, frames):
            return {'t': t, 'generation': 1, 'presentationVisible': True,
                    'presentedFrames': frames, 'presentedMediaTime': frames / 24,
                    'currentTime': frames / 24}
        first, middle, last = sample('1', 24), sample('2', 48), sample('3', 72)
        for unavailable in [{'t': '2', 'generation': 1},
                            dict(middle, presentedFrames=0),
                            dict(middle, presentedMediaTime=float('nan'))]:
            report = presentation_observations([{'driftSamples': [first, unavailable, last]}])
            self.assertEqual(report['progressingSameGenerationPairs'], 0)
            self.assertEqual(report['unmeasurableSameGenerationPairs'], 2)
        report = presentation_observations([{'driftSamples': [first, middle, last]},
            {'driftSamples': [dict(middle, presentationVisible=False)]}])
        self.assertEqual(report['progressingSameGenerationPairs'], 0)
        self.assertEqual(report['unmeasurableSameGenerationPairs'], 2)

    def test_visible_progress_requires_both_pair_endpoints_visible(self):
        samples = [{'t': str(index), 'generation': 1, 'presentationVisible': visible,
                    'presentedFrames': frames, 'presentedMediaTime': frames / 24,
                    'currentTime': frames / 24}
                   for index, frames, visible in [(1, 24, True), (2, 48, True),
                                                 (3, 72, False), (4, 96, True), (5, 96, True)]]
        report = presentation_observations([{'driftSamples': samples}])
        self.assertEqual(report['progressingSameGenerationPairs'], 3)
        self.assertEqual(report['visibleProgressingSameGenerationPairs'], 1)
        self.assertEqual(report['visibleNonadvancingSameGenerationPairs'], 1)
        self.assertEqual(report['unmeasurableSameGenerationPairs'], 0)

    def test_presentation_deduplicates_callbacks_not_media_clock(self):
        samples = [{'t': str(index), 'generation': 1, 'presentationVisible': True,
                    'presentedFrames': frames, 'presentedMediaTime': presented,
                    'currentTime': clock, 'presentationStallMs': stall}
                   for index, frames, presented, clock, stall in
                   [(1, 24, 1, 1.1, 0), (2, 48, 2, 2.1, 0),
                    (3, 48, 2, 5.1, 3000)]]
        report = presentation_observations([{'driftSamples': samples}] * 2)
        self.assertEqual(report['observedUniqueSamples'], 3)
        self.assertEqual(report['measurableSamples'], 3)
        self.assertEqual(report['visibleSamples'], 3)
        self.assertEqual(report['progressingSameGenerationPairs'], 1)
        self.assertEqual(report['nonadvancingSameGenerationPairs'], 1)
        self.assertAlmostEqual(report['maximumObservedClockAheadOfPresentationMs'], 3100)
        self.assertEqual(report['maximumObservedRunnableStallMs'], 3000)

    def test_presentation_unknown_invalid_and_conflicting_stay_unknown(self):
        sample = {'t': 'one', 'generation': 1, 'presentationVisible': True,
                  'presentedFrames': 24, 'presentedMediaTime': 1, 'currentTime': 1.1}
        conflict = dict(sample, presentationVisible=False)
        invalid = [dict(sample, t='bool', presentedFrames=True),
                   dict(sample, t='nan', presentedMediaTime=float('nan')),
                   dict(sample, t='zero', presentedFrames=0),
                   dict(sample, t='legacy', generation=None),
                   {'t': 'unknown', 'generation': 1}]
        report = presentation_observations([{'driftSamples': [sample, conflict, sample] + invalid}])
        self.assertEqual(report['observedUniqueSamples'], 5)
        self.assertEqual(report['measurableSamples'], 0)
        self.assertEqual(report['unknownVisibilitySamples'], 2)
        self.assertIsNone(report['maximumObservedClockAheadOfPresentationMs'])
        self.assertEqual(report['progressingSameGenerationPairs'], 0)
        self.assertEqual(presentation_observations([])['observedUniqueSamples'], 0)

    def test_presentation_generations_and_resets_do_not_invent_progress(self):
        samples = [{'t': str(index), 'generation': generation, 'presentationVisible': False,
                    'presentedFrames': frames, 'presentedMediaTime': presented,
                    'currentTime': presented}
                   for index, generation, frames, presented in
                   [(1, 1, 48, 2), (2, 1, 24, 1), (3, 2, 72, 3)]]
        report = presentation_observations([{'driftSamples': samples}])
        self.assertEqual(report['presentationResetPairs'], 1)
        self.assertEqual(report['progressingSameGenerationPairs'], 0)
        self.assertEqual(report['notVisibleSamples'], 3)

    def test_buffer_position_is_deduplicated_and_missing_fields_stay_unknown(self):
        samples = [{'t':'one','generation':1,'playheadBuffered':True},
                   {'t':'two','generation':1,'playheadBuffered':False,'playheadGapMs':1600,'nextBufferedStartSeconds':13},
                   {'t':'three','generation':1,'playheadBuffered':False,'bufferedRangeCount':0,'headroomInCurrentRangeMs':0},
                   {'t':'old','generation':1,'lagMs':99999}]
        report = buffer_position_observations([{'driftSamples':samples},{'driftSamples':samples}])
        self.assertEqual(report['observedUniqueSamples'],4)
        self.assertEqual(report['bufferedSamples'],1)
        self.assertEqual(report['gapSamples'],1)
        self.assertEqual(report['tailOrEmptySamples'],1)
        self.assertEqual(report['unknownPositionSamples'],1)
        self.assertEqual(report['maximumObservedGapMs'],1600)
        legacy = buffer_position_observations([{'driftSamples':[{'t':'old','lagMs':99999}]}])
        self.assertIsNone(legacy['maximumObservedGapMs'])
        self.assertEqual(legacy['unknownPositionSamples'],1)

    def test_conflicting_repeats_and_bad_gap_values_do_not_qualify(self):
        samples = [{'t':'same','playheadBuffered':True}, {'t':'same','playheadBuffered':False},
                   {'t':'nan','playheadBuffered':False,'playheadGapMs':float('nan'),'nextBufferedStartSeconds':13},
                   {'t':'bool','playheadBuffered':'true'}, {'t':'bad-generation','generation':[],'playheadBuffered':True}]
        report=buffer_position_observations([{'driftSamples':samples}])
        self.assertEqual(report['observedUniqueSamples'],3)
        self.assertEqual(report['unknownPositionSamples'],3)
        self.assertEqual(report['gapSamples'],0)
        self.assertEqual(report['bufferedSamples'],0)

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

    def test_legacy_http_collection_is_not_relabelled_as_summary_latency(self):
        replay, row = self.fixture()
        result = summarize(replay, {'history': [row]})
        self.assertEqual(result['healthCollectionWallMs']['p95'], 5)
        self.assertEqual(result['summaryHttpWallMs'], {'p50': None, 'p95': None, 'p99': None})
        self.assertFalse(result['summaryHttpTiming']['complete'])
        self.assertEqual(result['summaryHttpTiming']['availableSamples'], 0)

    def test_separate_summary_timing_requires_all_samples_and_explicit_scope(self):
        replay, row = self.fixture()
        scope = 'authenticatedHealthWithoutRawWorkSamplesRequestAndResponseBodyOnly'
        for index, sample in enumerate(replay['samples']):
            sample['httpMs'] = 100 + index
            sample['summaryHttpMs'] = 10 + index
            sample['summaryHttpScope'] = scope
        result = summarize(replay, {'history': [row]})
        self.assertEqual(result['healthCollectionWallMs']['p95'], 101)
        self.assertEqual(result['summaryHttpWallMs']['p95'], 11)
        self.assertTrue(result['summaryHttpTiming']['complete'])
        for invalid in [None, -1, True, float('nan'), float('inf')]:
            replay['samples'][1]['summaryHttpMs'] = invalid
            result = summarize(replay, {'history': [row]})
            self.assertFalse(result['summaryHttpTiming']['complete'])
            self.assertIsNone(result['summaryHttpWallMs']['p95'])
            self.assertEqual(result['summaryHttpTiming']['availableSamples'], 1)
        replay['samples'][1]['summaryHttpMs'] = 11
        replay['samples'][1]['summaryHttpScope'] = 'raw-export'
        self.assertIsNone(summarize(replay, {'history': [row]})['summaryHttpWallMs']['p95'])

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
