import unittest
from check_replay_sender_pacing import delivery_rate


class SenderPacingTests(unittest.TestCase):
    def test_excludes_startup_and_measures_media_slope(self):
        report = delivery_rate([{'wallSeconds': 0, 'mediaSeconds': 0},
                                {'wallSeconds': 10, 'mediaSeconds': 8},
                                {'wallSeconds': 30, 'mediaSeconds': 27}])
        self.assertEqual(report['wallSpanSeconds'], 20)
        self.assertEqual(report['mediaSpanSeconds'], 19)
        self.assertEqual(report['mediaPerWallSecond'], 0.95)

    def test_insufficient_or_non_advancing_progress_is_rejected(self):
        for samples in ([], [{'wallSeconds': 20, 'mediaSeconds': 1}],
                        [{'wallSeconds': 20, 'mediaSeconds': 1}, {'wallSeconds': 30, 'mediaSeconds': 1}]):
            with self.assertRaises(ValueError): delivery_rate(samples)


if __name__ == '__main__': unittest.main()
