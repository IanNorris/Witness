import copy
import unittest
from compare_replay_phases import compare


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
            'measurementWindow': {'observedDurationSeconds': 1800},
            'coverage': {'maximumGapSeconds': 1},
            'summaryHttpTiming': {'scope': 'authenticatedHealthWithoutRawWorkSamplesRequestAndResponseBodyOnly',
                                 'complete': True, 'availableSamples': 1800, 'totalSamples': 1800},
            'summaryHttpWallMs': {'p95': 20}, 'processCpuHostCapacityPercent': {'p50': 20, 'p95': 30},
            'processingTaskDistributions': [{'cameraId': i, 'complete': True, 'unavailableReasons': [],
                'scope': 'filterStageWallTimeExcludingQueueWaitAndDecode',
                'boundary': 'perCameraSnapshotSequenceCursors', 'p95Ms': 1} for i in range(1, 17)]}
        on = copy.deepcopy(off)
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

    def test_thresholds_are_strict_and_each_camera_is_checked(self):
        off, on, recordings = self.fixture()
        on['processCpuHostCapacityPercent']['p95'] += 1
        on['summaryHttpWallMs']['p95'] = 100
        on['processingTaskDistributions'][7]['p95Ms'] = 1.05
        report = compare(off, on, recordings, recordings)
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(len(report['thresholdFailures']), 3)

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
