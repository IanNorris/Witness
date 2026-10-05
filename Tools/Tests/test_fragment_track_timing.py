import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from check_fragment_track_timing import FragmentTiming, inspect, media_extents


class FragmentTrackTimingTests(unittest.TestCase):
    def test_boxes_and_extended_payloads(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.mp4'
            path.write_bytes(struct.pack('>I4s', 8, b'moof') +
                             struct.pack('>I4sQ', 1, b'mdat', 20) + b'abcd' +
                             struct.pack('>I4s', 8, b'moof') +
                             struct.pack('>I4s', 0, b'mdat') + b'xyz')
            self.assertEqual(media_extents(path), [(24, 28, 0), (44, 47, 1)])

    def test_rejects_invalid_and_nonfragmented_boxes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.mp4'
            for data in (b'short', struct.pack('>I4s', 100, b'moof'),
                         struct.pack('>I4s', 8, b'mdat'), struct.pack('>I4s', 1, b'mdat')):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    media_extents(path)

    def test_track_skew_gap_and_missing_track_are_separate(self):
        timing = FragmentTiming([(100, 200, 0), (300, 400, 1), (500, 600, 2)])
        for stream, pts, duration, pos in ((0, '0', '.050', 100), (1, '0', '.021333', 150),
                                           (0, '.050', '.050', 300), (1, '.064', '.021333', 350),
                                           (0, '.100', '.050', 500)):
            timing.add(f'packet|stream_index={stream}|pts_time={pts}|duration_time={duration}|pos={pos}\n'.encode())
        report = timing.report()
        self.assertEqual(report['packetCount'], 5)
        self.assertEqual(report['multiplexedFragmentCount'], 2)
        self.assertAlmostEqual(report['maximumTrackEndSpreadMs'], 28.667)
        self.assertAlmostEqual(report['fragments'][1]['tracks']['1']['gapFromPreviousTrackEndMs'], 42.667)
        self.assertIsNone(report['fragments'][2]['trackEndSpreadMs'])
        self.assertNotIn('1', report['fragments'][2]['tracks'])

    def test_rejects_bad_packet_records_and_positions(self):
        for record in ('frame|pos=100',
                       'packet|stream_index=0|pts_time=nan|duration_time=.1|pos=100',
                       'packet|stream_index=0|pts_time=0|duration_time=0|pos=100',
                       'packet|stream_index=0|pts_time=0|duration_time=.1|pos=200',
                       'packet|stream_index=0|pts_time=0|duration_time=.1|pos=100|pos=101'):
            with self.assertRaises(ValueError):
                FragmentTiming([(100, 200, 0)]).add(record.encode())

    def test_probe_errors_and_empty_output_never_become_success(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.mp4'
            path.write_bytes(struct.pack('>I4s', 8, b'moof') +
                             struct.pack('>I4s', 12, b'mdat') + b'abcd')
            for result in ((1, 0, False, ''), (0, 1, True, 'error'), (0, 0, False, '')):
                with patch('check_fragment_track_timing.bounded_process', return_value=result):
                    with self.assertRaises(ValueError):
                        inspect(path)
            def probe(command, consume, timeout):
                self.assertIn('file,pipe', command)
                self.assertEqual(timeout, 1800)
                consume(b'packet|stream_index=0|pts_time=0|duration_time=.05|pos=16\n')
                return 0, 0, False, ''
            with patch('check_fragment_track_timing.bounded_process', side_effect=probe):
                self.assertEqual(inspect(path)['packetCount'], 1)


if __name__ == '__main__':
    unittest.main()
