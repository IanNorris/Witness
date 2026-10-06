import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import check_long_recording as checker
from check_long_recording import TimestampAccumulator, bounded_process
from check_preview_decode import RecordingInspectionError


class LongRecordingTests(unittest.TestCase):
    def test_stage_progress_does_not_change_commands_or_results(self):
        for audio in (False, True):
            commands, stages = [], []
            def process(command, consume=None, timeout=1800, maximum_rows=2000000):
                commands.append(command)
                if 'stream=index' in command:
                    consume(b'stream|index=1\n')
                elif consume:
                    section, field = ('packet', 'dts') if '-show_packets' in command else ('frame', 'best_effort_timestamp')
                    for tick in (0, 1024):
                        consume(f'{section}|{field}={tick}\n'.encode())
                return 0, 0, False, ''
            inspect = checker.inspect_audio if audio else checker.inspect_long
            with self.subTest(audio=audio), patch.object(checker.Path, 'is_file', return_value=True), \
                 patch.object(checker.Path, 'stat', return_value=SimpleNamespace(st_size=1)), \
                 patch.object(checker, 'bounded_process', side_effect=process):
                baseline = inspect('owned-fixture.mp4')
                baseline_commands = commands[:]
                commands.clear()
                observed = inspect('owned-fixture.mp4', progress_callback=lambda phase, index: stages.append((phase, index)))
            self.assertEqual(observed, baseline)
            self.assertEqual(commands, baseline_commands)
            self.assertEqual(stages, [('audioTrackDiscovery', None), ('audioStrictDecode', 1),
                                     ('audioPackets', 1), ('audioFrames', 1)] if audio else
                                    [('videoStrictDecode', None), ('videoPackets', None), ('videoFrames', None)])

    def test_invalid_stage_callback_fails_before_access_or_launch(self):
        for inspect in (checker.inspect_long, checker.inspect_audio):
            with patch.object(checker.Path, 'is_file') as exists, \
                 patch.object(checker, 'bounded_process') as run, self.assertRaises(ValueError):
                inspect('owned-fixture.mp4', progress_callback=True)
            exists.assert_not_called()
            run.assert_not_called()

    def test_audio_checks_each_track_and_does_not_claim_absent_audio(self):
        for indices in ([], [1, 2]):
            commands = []
            def process(command, consume=None, timeout=1800, maximum_rows=2000000):
                commands.append(command)
                if 'stream=index' in command:
                    for index in indices:
                        consume(f'program|stream|index={index}\n'.encode())
                    consume(b'\n')
                    for index in indices:
                        consume(f'stream|index={index}\n'.encode())
                elif consume:
                    section, field = ('packet', 'dts') if '-show_packets' in command else ('frame', 'best_effort_timestamp')
                    for tick in (0, 1024):
                        consume(f'{section}|{field}={tick}\n'.encode())
                else:
                    self.assertIn('-xerror', command)
                    self.assertNotIn('-t', command)
                    self.assertNotIn('-ss', command)
                return 0, 0, False, ''
            with self.subTest(indices=indices), patch.object(checker.Path, 'is_file', return_value=True), \
                 patch.object(checker.Path, 'stat', return_value=SimpleNamespace(st_size=1)), \
                 patch.object(checker, 'bounded_process', side_effect=process):
                report = checker.inspect_audio('owned-fixture.mp4')
            self.assertTrue(report['passed'])
            self.assertEqual(report['audioPresent'], bool(indices))
            self.assertEqual([t['streamIndex'] for t in report['tracks']], indices)
            self.assertEqual(len(commands), 1 + 3 * len(indices))

    def test_audio_decode_and_timestamp_faults_fail(self):
        for fault in ('decode', 'timestamp', 'diagnostic'):
            def process(command, consume=None, timeout=1800, maximum_rows=2000000):
                if 'stream=index' in command:
                    consume(b'stream|index=1\n')
                elif consume:
                    section, field = ('packet', 'dts') if '-show_packets' in command else ('frame', 'best_effort_timestamp')
                    for tick in (0, 0 if fault == 'timestamp' else 1024):
                        consume(f'{section}|{field}={tick}\n'.encode())
                    return 0, int(fault == 'diagnostic'), fault == 'diagnostic', ''
                elif fault == 'decode':
                    return 1, 1, True, 'decode error'
                return 0, 0, False, ''
            with self.subTest(fault=fault), patch.object(checker.Path, 'is_file', return_value=True), \
                 patch.object(checker.Path, 'stat', return_value=SimpleNamespace(st_size=1)), \
                 patch.object(checker, 'bounded_process', side_effect=process):
                self.assertFalse(checker.inspect_audio('owned-fixture.mp4')['passed'])

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
