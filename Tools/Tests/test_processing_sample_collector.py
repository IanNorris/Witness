import unittest
from processing_sample_collector import ProcessingSampleCollector, SCOPE


def health(total, values, first=None, epoch=1):
    first = total - len(values) + 1 if values and first is None else (first or 0)
    return {'cameras': [{'cameraId': 1, 'processing': {'workLatency': {
        'epoch': epoch, 'samplesSinceReset': total, 'firstSequence': first,
        'samples': len(values),
        'rawSamplesNS': values, 'scope': SCOPE, 'capacity': 1024}}}]}


class ProcessingCollectorTests(unittest.TestCase):
    def test_overlaps_deduplicate_and_exact_distribution_is_not_rolling(self):
        collector = ProcessingSampleCollector(health(2, [100, 200]))
        collector.add(health(4, [100, 200, 1000000, 4000000]))
        collector.add(health(5, [100, 200, 1000000, 4000000, 2000000]))
        result = collector.report()[0]
        self.assertTrue(result['complete'])
        self.assertEqual(result['retainedSamples'], 3)
        self.assertEqual((result['p50Ms'], result['p95Ms'], result['p99Ms']), (2, 4, 4))

    def test_overrun_is_not_a_full_phase_percentile(self):
        collector = ProcessingSampleCollector(health(1, [1]))
        collector.add(health(1100, [1] * 1024))
        result = collector.report()[0]
        self.assertFalse(result['complete'])
        self.assertIn('ringOverrun', result['unavailableReasons'])
        self.assertIsNone(result['p95Ms'])

    def test_epoch_and_counter_resets_are_not_silently_combined(self):
        for epoch in (1, 2):
            collector = ProcessingSampleCollector(health(10, [1] * 10))
            collector.add(health(2, [1, 2], epoch=epoch))
            self.assertFalse(collector.report()[0]['complete'])

    def test_capacity_is_bounded_and_overflow_stays_unqualified(self):
        collector = ProcessingSampleCollector(health(0, []), capacity_per_camera=2)
        collector.add(health(3, [1, 2, 3]))
        result = collector.report()[0]
        self.assertEqual(result['retainedSamples'], 2)
        self.assertIsNone(result['p95Ms'])

    def test_zero_timings_are_valid_but_empty_phase_is_unavailable(self):
        collector = ProcessingSampleCollector(health(0, []))
        self.assertFalse(collector.report()[0]['complete'])
        collector.add(health(1, [0]))
        self.assertTrue(collector.report()[0]['complete'])
        self.assertEqual(collector.report()[0]['p95Ms'], 0)

    def test_inconsistent_shape_negative_boolean_and_missing_samples_rejected(self):
        for sample in (health(2, [1], first=1), health(1, [-1]), health(1, [True]),
                       health(1, [2**63]), health(1, [])):
            with self.assertRaises(ValueError):
                ProcessingSampleCollector(sample)

    def test_camera_change_rejected(self):
        collector = ProcessingSampleCollector(health(0, []))
        changed = health(1, [1])
        changed['cameras'][0]['cameraId'] = 2
        with self.assertRaises(ValueError):
            collector.add(changed)


if __name__ == '__main__':
    unittest.main()
