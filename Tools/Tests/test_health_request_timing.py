import json
import unittest
from unittest.mock import patch
from health_request_timing import sample_health, SUMMARY_SCOPE


def response(raw=False, native='native', camera=1):
    work = {'rawSamplesNS': [10]} if raw else {}
    return json.dumps({'server': {'nativeBuildHash': native, 'webBuildHash': 'web'},
                       'cameras': [{'cameraId': camera, 'processing': {'workLatency': work}}]}).encode()


class HealthRequestTimingTests(unittest.TestCase):
    def test_raw_and_summary_are_independently_timed_before_json_parsing(self):
        paths, ticks = [], iter([1, 1.020, 2, 2.005])
        def request(path):
            paths.append(path)
            return 200, response(raw=path.endswith('=1'))
        health, timing = sample_health(request, True, lambda: next(ticks))
        self.assertEqual(paths, ['/debug/health?workSamples=1', '/debug/health?workSamples=0'])
        self.assertEqual(health['cameras'][0]['processing']['workLatency']['rawSamplesNS'], [10])
        self.assertAlmostEqual(timing['httpMs'], 20)
        self.assertAlmostEqual(timing['summaryHttpMs'], 5)
        self.assertEqual(timing['summaryHttpScope'], SUMMARY_SCOPE)

    def test_ordinary_mode_uses_one_request(self):
        paths, ticks = [], iter([1, 1.010])
        def request(path):
            paths.append(path)
            return 200, response()
        _, timing = sample_health(request, False, lambda: next(ticks))
        self.assertEqual(paths, ['/debug/health?workSamples=0'])
        self.assertEqual(timing['httpMs'], timing['summaryHttpMs'])
        self.assertFalse(timing['healthHttpIncludesRawSamples'])

    def test_all_http_clock_reads_precede_json_decode(self):
        events = []
        def clock():
            events.append('clock')
            return len(events)
        def request(path):
            events.append('request')
            return 200, response(raw=path.endswith('=1'))
        decode = json.loads
        def loads(body):
            self.assertEqual(events, ['clock', 'request', 'clock'] * 2)
            return decode(body)
        with patch('health_request_timing.json.loads', side_effect=loads):
            sample_health(request, True, clock)

    def test_invalid_clock_interval_is_not_a_fast_request(self):
        for stop in [0, float('nan'), float('inf')]:
            ticks = iter([1, stop])
            with self.assertRaises(ValueError):
                sample_health(lambda _: (200, response()), False, lambda: next(ticks))

    def test_auth_failure_raw_leak_and_identity_membership_changes_fail_closed(self):
        for replies in [[(403, b'')], [(200, response(True)), (200, response(True))],
                        [(200, response(True)), (200, response(native='other'))],
                        [(200, response(True)), (200, response(camera=2))]]:
            with self.subTest(replies=len(replies)):
                iterator = iter(replies)
                with self.assertRaises(ValueError):
                    sample_health(lambda _: next(iterator), True)


if __name__ == '__main__':
    unittest.main()
