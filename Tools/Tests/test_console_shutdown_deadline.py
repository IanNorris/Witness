"""Offline timing contract; no server, console attachment or camera access."""
import subprocess
from pathlib import Path
import unittest
from unittest.mock import MagicMock, Mock, patch

from test_full_server_isolation import replay_senders, wait_for_console_exit


class ConsoleShutdownDeadlineTests(unittest.TestCase):
    def test_fast_exit_needs_no_snapshot(self):
        process, snapshot = Mock(), Mock()
        with patch('test_full_server_isolation.time.monotonic', return_value=100):
            wait_for_console_exit(process, snapshot)
        process.wait.assert_called_once_with(timeout=10)
        snapshot.assert_not_called()

    def test_snapshot_time_does_not_extend_original_deadline(self):
        process, snapshot = Mock(), Mock()
        process.wait.side_effect = [subprocess.TimeoutExpired('owned-child', 10), 0]
        with patch('test_full_server_isolation.time.monotonic', side_effect=[100, 112]):
            wait_for_console_exit(process, snapshot)
        self.assertEqual([call.kwargs['timeout'] for call in process.wait.call_args_list], [10, 18])
        snapshot.assert_called_once_with()

    def test_timeout_is_not_suppressed_or_given_another_thirty_seconds(self):
        process, snapshot = Mock(), Mock()
        process.wait.side_effect = [subprocess.TimeoutExpired('owned-child', 10),
                                    subprocess.TimeoutExpired('owned-child', 0)]
        with patch('test_full_server_isolation.time.monotonic', side_effect=[100, 131]):
            with self.assertRaises(subprocess.TimeoutExpired):
                wait_for_console_exit(process, snapshot)
        self.assertEqual([call.kwargs['timeout'] for call in process.wait.call_args_list], [10, 0])

    def test_without_snapshot_retains_original_wait(self):
        process = Mock()
        process.wait.side_effect = subprocess.TimeoutExpired('owned-child', 30)
        with patch('test_full_server_isolation.time.monotonic', return_value=100):
            with self.assertRaises(subprocess.TimeoutExpired):
                wait_for_console_exit(process)
        process.wait.assert_called_once_with(timeout=30)

    def test_replay_sender_does_not_inherit_detached_console_input(self):
        child = Mock()
        child.poll.return_value = 0
        with patch.object(Path, 'open', return_value=MagicMock()):
            with patch('test_full_server_isolation.subprocess.Popen', return_value=child) as spawn:
                with replay_senders(['private-test.mp4'], ['http://127.0.0.1:1'], Path('offline-fixture')):
                    self.assertEqual(spawn.call_args.kwargs['stdin'], subprocess.DEVNULL)
        child.wait.assert_called_once_with(timeout=10)


if __name__ == '__main__':
    unittest.main()
