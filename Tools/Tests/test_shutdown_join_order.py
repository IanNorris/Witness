"""Production-source ordering guard, not a runtime blocking-callback test."""
from pathlib import Path
import re
import unittest


class ShutdownJoinOrderTests(unittest.TestCase):
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
