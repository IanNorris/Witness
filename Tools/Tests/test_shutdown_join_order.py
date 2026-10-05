"""Production-source ordering guard, not a runtime blocking-callback test."""
from pathlib import Path
import re
import unittest


class ShutdownJoinOrderTests(unittest.TestCase):
    def test_unsigned_clip_end_timestamps_use_zero_not_negative_sentinels(self):
        root = Path(__file__).resolve().parents[2] / 'WitnessServer/Source'
        source = re.sub(r'//[^\n]*|/\*.*?\*/', '',
                        (root / 'ObservingMotionFilter.cpp').read_text(encoding='utf8'), flags=re.S)
        for field in ('TimestampMotionEnded', 'TimestampClipEnded'):
            self.assertNotRegex(source, r'ClipStats\.' + field + r'\s*=\s*INT64_MIN')
            self.assertRegex(source, r'ClipStats\.' + field + r'\s*=\s*0;')
        recording = (root / 'Recording.cpp').read_text(encoding='utf8')
        self.assertIn('FinalizedMotionDurationSeconds(', recording)
        self.assertNotIn('ClipStats.TimestampMotionEnded - ClipStats.TimestampMotionStarted', recording)

    def test_final_clip_metadata_drain_is_serialized_after_all_worker_joins(self):
        root = Path(__file__).resolve().parents[2] / 'WitnessServer/Source'
        source = (root / 'Shutdown.cpp').read_text(encoding='utf8')
        request = source.split('void WitnessServer::RequestShutdown()', 1)[1]
        request = re.sub(r'//[^\n]*|/\*.*?\*/', '', request, flags=re.S)
        drain = request.index('MessageClient->TakePending()')
        for joined in ('Camera.second.Worker->Join();', 'if( Worker ) Worker->Join();',
                       'if( Watchdog ) Watchdog->Join();', 'if( Timer ) Timer->Join();'):
            self.assertLess(request.index(joined), drain)
        self.assertLess(request.index('std::lock_guard DispatchLock(MessageDispatchMutex);'), drain)
        self.assertIn('StopCameraRecording(Data.ClipStats, Data.Camera, Data.Result);', request[drain:])
        self.assertNotIn('StartCameraRecording(', request[drain:])
        self.assertIn('MessageClient->Push(std::make_shared<ThreadShutdownMessage>());', request[drain:])
        dispatcher = (root / 'MessageDispatcher.cpp').read_text(encoding='utf8')
        self.assertLess(dispatcher.index('std::lock_guard DispatchLock(MessageDispatchMutex);'),
                        dispatcher.index('Msg->Handle<CameraClipFinishedMessage>'))

    def test_camera_finalizes_active_recording_before_derived_state_teardown(self):
        source = (Path(__file__).resolve().parents[2] /
                  'WitnessServer/Source/CameraWorker.cpp').read_text(encoding='utf-8')
        shutdown = source.split('void CameraWorker::WorkerShutdown()', 1)[1].split(
            'void CameraWorker::WorkerMain()', 1)[0]
        shutdown = re.sub(r'//[^\n]*|/\*.*?\*/', '', shutdown, flags=re.S)
        finish = shutdown.index('OnClipFinished(true);')
        for teardown in ('Filter = nullptr;', 'ContinuousStream = nullptr;',
                         'LiveStream = nullptr;', 'RetiredCameraStream.reset();'):
            self.assertLess(finish, shutdown.index(teardown))
        self.assertLess(finish, shutdown.index('std::make_shared<ThreadShutdownMessage>()'))

    def test_background_workers_join_before_request_shutdown_returns(self):
        source = (Path(__file__).resolve().parents[2] /
                  'WitnessServer/Source/Shutdown.cpp').read_text(encoding='utf-8')
        request = source.split('void WitnessServer::RequestShutdown()', 1)[1]
        # Do not count comments as evidence of a production call.
        request = re.sub(r'//[^\n]*|/\*.*?\*/', '', request, flags=re.S)
        request = re.sub(r'"(?:\\.|[^"\\])*"', '""', request)
        start = request.index('{')
        depth = 0
        for index in range(start, len(request)):
            depth += (request[index] == '{') - (request[index] == '}')
            if depth == 0:
                request = request[start:index + 1]
                break
        else:
            self.fail('RequestShutdown body could not be bounded')
        queue_stop = request.index('CommonImageProcessingJobQueue.RequestShutdown();')
        for worker in ('Worker', 'Watchdog', 'Timer'):
            with self.subTest(worker=worker):
                stop = request.index(worker + '->RequestShutdown();')
                joined = re.search(r'if\s*\(\s*' + worker + r'\s*\)\s*' +
                                   worker + r'->Join\(\);', request)
                self.assertIsNotNone(joined, worker + ' must join explicitly before teardown')
                self.assertGreater(joined.start(), stop)
                self.assertGreater(joined.start(), queue_stop)


if __name__ == '__main__':
    unittest.main()
