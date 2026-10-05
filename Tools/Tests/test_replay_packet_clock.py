import unittest

from check_replay_packet_clock import PacketClock


class PacketClockTests(unittest.TestCase):
    def test_distinguishes_padding_overlap_from_backward_dts(self):
        clock = PacketClock()
        for dts, duration in (('0', '.05'), ('.05', '.05'), ('.1', '.05'), ('.14', '.05'), ('.12', '.05')):
            clock.add(f'packet|stream_index=0|pts_time={dts}|dts_time={dts}|duration_time={duration}|\n'.encode())
        report = clock.report()['streams']['0']
        self.assertEqual(report['regressions'], 1)
        self.assertEqual(report['overlapsOver2Ms'], 2)
        self.assertAlmostEqual(report['elapsedMinusDurationSumMs'], -80)
        self.assertAlmostEqual(report['minimumGapMs'], -70)

    def test_track_clocks_remain_separate_and_anomalies_bounded(self):
        clock = PacketClock()
        for i in range(50):
            for stream, duration in ((0, '.05'), (1, '.02')):
                clock.add(f'packet|stream_index={stream}|pts_time={i}|dts_time={i}|duration_time={duration}|side_data|id=224\n'.encode())
        clock.add(b'\n')
        for state in clock.report()['streams'].values():
            self.assertEqual(state['packets'], 50)
            self.assertEqual(state['regressions'], 0)
            self.assertEqual(state['gapsOver2Ms'], 49)
            self.assertEqual(len(state['largestDiscontinuities']), 16)

    def test_rejects_invalid_clock_fields(self):
        for record in ('frame|stream_index=0',
                       'packet|stream_index=32|pts_time=0|dts_time=0|duration_time=.1',
                       'packet|stream_index=0|pts_time=nan|dts_time=0|duration_time=.1',
                       'packet|stream_index=0|pts_time=0|dts_time=0|duration_time=0',
                       'packet|stream_index=0|pts_time=0|dts_time=0|duration_time=.1|dts_time=1'):
            with self.assertRaises(ValueError):
                PacketClock().add(record.encode())


if __name__ == '__main__':
    unittest.main()
