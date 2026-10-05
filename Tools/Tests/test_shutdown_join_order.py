"""Production-source ordering guard, not a runtime blocking-callback test."""
from pathlib import Path
import re
import unittest


class ShutdownJoinOrderTests(unittest.TestCase):
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
