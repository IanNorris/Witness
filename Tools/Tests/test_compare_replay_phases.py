import copy
import unittest
from compare_replay_phases import compare
from replay_source_identity import SCOPE


class ReplayPhaseComparisonTests(unittest.TestCase):
    def fixture(self):
        off = {'nativeBuildId': 'native', 'webBuildId': 'web', 'cameraCount': 16,
            'sampleCount': 1800, 'browserReadyBeforePhase': True,
            'workload': {'objectDetection': True, 'provider': 'cpu', 'maximumMotionInferenceFps': 1,
                         'recording': 'continuous DVR plus authenticated manual clips',
                         'strictRecordingInspection': True},
            'mainStreamProfiles': [{'cameraId': i, 'videoCodec': 'h264' if i % 2 else 'hevc',
                                   'width': 1920, 'height': 1080, 'audioCodec': ''} for i in range(1, 17)],
            'observerModes': dict.fromkeys(('http', 'database', 'packet', 'mux', 'media'), False),
            'measurementWindow': {'startedUnixMs': 2000, 'observedDurationSeconds': 1800},
            'coverage': {'maximumGapSeconds': 1},
            'sampledBrowserVisibility': [{'scope': 'authenticatedSessionReceiptsWithinMeasurementWindowNotContinuousVisibilityProof',
                'retainedRows': 121, 'hiddenRows': 0, 'unknownVisibilityRows': 0, 'incompletePlayerRows': 0,
                'startOffsetSeconds': 0, 'endGapSeconds': 0, 'maximumReceiptGapSeconds': 15}],
            'summaryHttpTiming': {'scope': 'authenticatedHealthWithoutRawWorkSamplesRequestAndResponseBodyOnly',
                                 'complete': True, 'availableSamples': 1800, 'totalSamples': 1800},
            'summaryHttpWallMs': {'p95': 20}, 'processCpuHostCapacityPercent': {'p50': 20, 'p95': 30},
            'processingTaskDistributions': [{'cameraId': i, 'complete': True, 'unavailableReasons': [],
                'scope': 'filterStageWallTimeExcludingQueueWaitAndDecode',
                'boundary': 'perCameraSnapshotSequenceCursors', 'p95Ms': 1} for i in range(1, 17)]}
        off['browserLayoutEvidence'] = {'validatedUnixMs': 1500, 'proof': {
            'capturedUtc': '1970-01-01T00:00:01Z',
            'viewport': {'width': 2172, 'height': 1550},
            'visibility': 'visible', 'browserPresented': True, 'fullscreen': True, 'activityVisible': False,
            'players': [{'width': 1920, 'height': 1080, 'insideViewport': True,
                         'paused': False, 'readyState': 4, 'error': None, 'currentTime': 10}
                        for _ in range(16)]}}
        on = copy.deepcopy(off)
        source_rows = [{'cameraId': i, 'bytes': 1000, 'sha256': 'a' * 64} for i in range(1, 17)]
        off['replaySourceIdentity'] = {'scope': SCOPE, 'before': source_rows,
                                      'after': copy.deepcopy(source_rows), 'unchanged': True}
        on['replaySourceIdentity'] = copy.deepcopy(off['replaySourceIdentity'])
        on['observerModes'] = dict.fromkeys(off['observerModes'], True)
        rows = [{'cameraId': i, 'kind': kind, 'passed': True}
                for kind in ('clip', 'continuous') for i in range(1, 17)]
        recordings = {'inspectionComplete': True, 'passed': True, 'expectedOutputs': 32,
                      'completedOutputs': 32, 'recordingInspection': rows}
        return off, on, recordings

    def test_valid_identical_workload_reports_numeric_scope_not_stability(self):
        off, on, recordings = self.fixture()
        report = compare(off, on, recordings, recordings)
        self.assertEqual(report['status'], 'numericThresholdsMet')
        self.assertEqual(len(report['processing']), 16)
        self.assertIn('Not production stability', report['limits'][0])

    def test_expected_audio_requires_its_own_completed_inspection(self):
        off, on, recordings = self.fixture()
        off['mainStreamProfiles'][3]['audioCodec'] = 'aac'
        on['mainStreamProfiles'][3]['audioCodec'] = 'aac'
        report = compare(off, on, recordings, recordings)
        self.assertEqual(report['status'], 'incomplete')
        self.assertIn('off expected recording audio inspection missing or failed', report['incompleteReasons'])
        for row in recordings['recordingInspection']:
            if row['cameraId'] == 4:
                row['audio'] = dict(inspectionMode='boundedStreamingFullAudio', required=True,
                                    audioPresent=True, passed=True, tracks=[{'passed': True}])
        self.assertEqual(compare(off, on, recordings, recordings)['status'], 'numericThresholdsMet')
        recordings['recordingInspection'][3]['audio']['tracks'][0]['passed'] = False
        self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')

    def test_thresholds_are_strict_and_each_camera_is_checked(self):
        off, on, recordings = self.fixture()
        on['processCpuHostCapacityPercent']['p95'] += 1
        on['summaryHttpWallMs']['p95'] = 100
        on['processingTaskDistributions'][7]['p95Ms'] = 1.05
        report = compare(off, on, recordings, recordings)
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(len(report['thresholdFailures']), 3)

    def test_same_profiles_cannot_hide_different_or_unverified_input_bytes(self):
        off, on, recordings = self.fixture()
        on['replaySourceIdentity']['before'][0]['sha256'] = 'b' * 64
        on['replaySourceIdentity']['after'][0]['sha256'] = 'b' * 64
        report = compare(off, on, recordings, recordings)
        self.assertEqual(report['status'], 'incomplete')
        self.assertIn('mismatched whole-file replay sources', report['incompleteReasons'])
        for change in (lambda p: p.pop('replaySourceIdentity'),
                       lambda p: p['replaySourceIdentity'].update(unchanged=False),
                       lambda p: p['replaySourceIdentity']['after'].pop(),
                       lambda p: p['mainStreamProfiles'][0].update(cameraId=True)):
            off, on, recordings = self.fixture()
            change(on)
            self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')

    def test_hidden_unknown_sparse_or_partial_sessions_cannot_qualify(self):
        for field, value in [('hiddenRows', 1), ('unknownVisibilityRows', 1),
                             ('incompletePlayerRows', 1), ('retainedRows', 1),
                             ('maximumReceiptGapSeconds', 31), ('startOffsetSeconds', 31),
                             ('endGapSeconds', None), ('hiddenRows', False),
                             ('scope', 'legacyUnanchoredHistory')]:
            off, on, recordings = self.fixture()
            on['sampledBrowserVisibility'][0][field] = value
            with self.subTest(field=field, value=value):
                self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')
        off, on, recordings = self.fixture()
        on.pop('sampledBrowserVisibility')
        self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')

    def test_browser_proof_must_be_recent_visible_complete_and_same_viewport(self):
        for change in [lambda p: p.pop('browserLayoutEvidence'),
                       lambda p: p.update(browserLayoutEvidence=None),
                       lambda p: p['browserLayoutEvidence']['proof'].update(viewport={'width': 1280, 'height': 720}),
                       lambda p: p['browserLayoutEvidence']['proof'].update(viewport={'width': True, 'height': 720}),
                       lambda p: p['browserLayoutEvidence']['proof'].update(visibility='hidden'),
                       lambda p: p['browserLayoutEvidence']['proof'].pop('browserPresented'),
                       lambda p: p['browserLayoutEvidence']['proof'].update(browserPresented=False),
                       lambda p: p['browserLayoutEvidence']['proof'].update(activityVisible=True),
                       lambda p: p['browserLayoutEvidence']['proof']['players'].pop(),
                       lambda p: p['browserLayoutEvidence']['proof']['players'][0].update(readyState=2),
                       lambda p: p['browserLayoutEvidence']['proof'].update(capturedUtc='invalid'),
                       lambda p: p['browserLayoutEvidence'].update(validatedUnixMs=True),
                       lambda p: p['browserLayoutEvidence'].update(validatedUnixMs=3000),
                       lambda p: p['measurementWindow'].update(startedUnixMs=62000),
                       lambda p: p['measurementWindow'].pop('startedUnixMs')]:
            off, on, recordings = self.fixture()
            change(on)
            with self.subTest(change=change):
                self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')

    def test_phase_capture_times_need_not_match_but_each_proof_must_qualify(self):
        off, on, recordings = self.fixture()
        on['measurementWindow']['startedUnixMs'] = 86402000
        on['browserLayoutEvidence']['validatedUnixMs'] = 86401500
        on['browserLayoutEvidence']['proof']['capturedUtc'] = '1970-01-02T00:00:01Z'
        self.assertEqual(compare(off, on, recordings, recordings)['status'], 'numericThresholdsMet')

    def test_missing_nonfinite_and_incomplete_evidence_never_pass(self):
        for change in [lambda p: p.update(nativeBuildId='different'),
                       lambda p: p.update(mainStreamProfiles=None),
                       lambda p: p['observerModes'].update(packet=None),
                       lambda p: p['measurementWindow'].update(observedDurationSeconds=1799),
                       lambda p: p['coverage'].update(maximumGapSeconds=11),
                       lambda p: p['summaryHttpWallMs'].update(p95=float('nan')),
                       lambda p: p['processCpuHostCapacityPercent'].update(p50=True),
                       lambda p: p['processingTaskDistributions'][0].update(complete=False),
                       lambda p: p['processingTaskDistributions'].pop(),
                       lambda p: p['workload'].update(objectDetection=False)]:
            off, on, recordings = self.fixture()
            change(on)
            self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')
        off, on, recordings = self.fixture()
        recordings['inspectionComplete'] = False
        self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')
        recordings['inspectionComplete'] = True
        recordings['recordingInspection'][-1]['cameraId'] = 1
        self.assertEqual(compare(off, on, recordings, recordings)['status'], 'incomplete')


if __name__ == '__main__':
    unittest.main()
