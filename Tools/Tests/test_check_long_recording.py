import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import check_long_recording as checker
from check_long_recording import TimestampAccumulator, bounded_process
from check_preview_decode import RecordingInspectionError


class LongRecordingTests(unittest.TestCase):
    def test_large_file_uses_full_strict_decode_and_both_timestamp_passes(self):
        def process(command, consume=None, timeout=1800):
            self.assertIn('file,pipe', command)
            self.assertEqual(timeout, 1800)
            if consume:
                section, field = ('packet', 'dts') if '-show_packets' in command else ('frame', 'best_effort_timestamp')
                for tick in (0, 4500):
                    consume(f'{section}|{field}={tick}\n'.encode())
            else:
                self.assertIn('-xerror', command)
                self.assertIn('1:1000000', command)
                self.assertNotIn('-t', command)
                self.assertNotIn('-ss', command)
            return 0, 0, False, ''
        with patch.object(checker.Path, 'is_file', return_value=True), \
             patch.object(checker.Path, 'stat', return_value=SimpleNamespace(st_size=600 * 1024**2)), \
             patch.object(checker, 'bounded_process', side_effect=process) as run:
            report = checker.inspect_long('owned-fixture.mp4')
        self.assertTrue(report['passed'])
        self.assertEqual(run.call_count, 3)

    def test_errors_and_missing_timestamps_still_fail_qualification(self):
        for code, diagnostics, missing in ((1, False, False), (0, True, False), (0, False, True)):
            def process(command, consume=None, timeout=1800):
                if consume:
                    section, field = ('packet', 'dts') if '-show_packets' in command else ('frame', 'best_effort_timestamp')
                    for tick in ('0', 'N/A' if missing else '4500'):
                        consume(f'{section}|{field}={tick}\n'.encode())
                    return 0, 0, False, ''
                return code, int(diagnostics), diagnostics, 'error' if diagnostics else ''
            with self.subTest(code=code, diagnostics=diagnostics, missing=missing), \
                 patch.object(checker.Path, 'is_file', return_value=True), \
                 patch.object(checker.Path, 'stat', return_value=SimpleNamespace(st_size=1)), \
                 patch.object(checker, 'bounded_process', side_effect=process):
                self.assertFalse(checker.inspect_long('owned-fixture.mp4')['passed'])

    def test_opt_in_threads_change_only_full_frame_probe_and_are_recorded(self):
        commands = []
        def process(command, consume=None, timeout=1800):
            commands.append(command)
            if consume:
                section, field = ('packet', 'dts') if '-show_packets' in command else ('frame', 'best_effort_timestamp')
                for tick in (0, 4500):
                    consume(f'{section}|{field}={tick}\n'.encode())
            self.assertNotIn('-t', command)
            self.assertNotIn('-ss', command)
            return 0, 0, False, ''
        with patch.object(checker.Path, 'is_file', return_value=True), \
             patch.object(checker.Path, 'stat', return_value=SimpleNamespace(st_size=1)), \
             patch.object(checker, 'bounded_process', side_effect=process):
            report = checker.inspect_long('owned-fixture.mp4', frame_decoder_threads=4)
        self.assertTrue(report['passed'])
        self.assertEqual(report['frameDecoderThreads'], 4)
        self.assertNotIn('-threads', commands[0])
        self.assertNotIn('-threads', commands[1])
        self.assertEqual(commands[2][commands[2].index('-threads') + 1], '4')
        self.assertIn('-show_frames', commands[2])

    def test_invalid_thread_limits_fail_before_launch_or_file_access(self):
        for value in (0, 9, -1, True, 4.0, '4', None):
            with self.subTest(value=value), patch.object(checker, 'bounded_process') as run, \
                 patch.object(checker.Path, 'is_file') as exists, self.assertRaises(ValueError):
                checker.inspect_long('owned-fixture.mp4', frame_decoder_threads=value)
            run.assert_not_called()
            exists.assert_not_called()

    def test_streaming_timestamps_keep_all_faults_and_ignore_side_data(self):
        accumulator = TimestampAccumulator('frame', 'best_effort_timestamp')
        for line in (b'frame|best_effort_timestamp=10|side_data|type=test\n',
                     b'frame|best_effort_timestamp=10\n', b'frame|best_effort_timestamp=9\n',
                     b'frame|best_effort_timestamp=N/A\n', b'frame\n'):
            accumulator.add(line)
        self.assertEqual(accumulator.report(), dict(samples=3, missing=2, duplicates=1,
                                                   regressions=1, minimumStepTicks=-1))

    def test_malformed_rows_are_not_dropped(self):
        for line in (b'stream|dts=1\n', b'packet|dts=x\n', b'packet|dts=1|dts=2\n'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                TimestampAccumulator('packet', 'dts').add(line)

    def test_stderr_is_drained_but_bounded(self):
        code, lines, nonempty, diagnostic = bounded_process(
            [sys.executable, '-c', "import sys; sys.stderr.write('error\\n'*20000); print('packet|dts=1')"])
        self.assertEqual(code, 0)
        self.assertEqual(lines, 20000)
        self.assertTrue(nonempty)
        self.assertEqual(len(diagnostic), 16384)

    def test_large_timestamp_stream_is_fully_counted_incrementally(self):
        accumulator = TimestampAccumulator('packet', 'dts')
        code, lines, nonempty, _ = bounded_process(
            [sys.executable, '-c', "import sys; [sys.stdout.write('packet|dts=%d\\n' % i) for i in range(250000)]"],
            accumulator.add, timeout=30)
        self.assertEqual((code, lines, nonempty), (0, 0, False))
        self.assertEqual(accumulator.report(), dict(samples=250000, missing=0,
            duplicates=0, regressions=0, minimumStepTicks=1))

    def test_overlong_and_excess_rows_fail(self):
        for script, limit in (("print('x'*10000)", 10), ("print('x\\n'*20)", 2)):
            with self.subTest(script=script), self.assertRaises(ValueError):
                bounded_process([sys.executable, '-c', script], maximum_rows=limit)

    def test_timeout_kills_and_reaps_owned_child(self):
        with self.assertRaises(RecordingInspectionError) as caught:
            bounded_process([sys.executable, '-c', 'import time; time.sleep(30)'], timeout=.1)
        self.assertEqual(caught.exception.stage, 'timeout')


if __name__ == '__main__':
    unittest.main()
