"""Windows subprocess policy without changing inspection command semantics."""
import io
import ast
from pathlib import Path
import unittest
from unittest.mock import Mock, patch
import check_long_recording as checker
import quiet_subprocess


class QuietSubprocessTests(unittest.TestCase):
    def test_background_tool_launches_explicitly_suppress_windows(self):
        root = Path(__file__).parent
        for name in ('check_long_recording.py', 'check_preview_decode.py', 'prepare_replay_media.py',
                     'run_ptz_physical_probe.py', 'run_ptz_capability_probe.py', 'run_live_preview_probe.py',
                     'run_client_health_browser_fixture.py', 'benchmark_client_health_http.py'):
            calls = [node for node in ast.walk(ast.parse((root / name).read_text(encoding='utf-8')))
                     if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                     and isinstance(node.func.value, ast.Name) and node.func.value.id == 'subprocess'
                     and node.func.attr in ('Popen', 'run')]
            self.assertTrue(calls, name)
            for call in calls:
                with self.subTest(script=name, line=call.lineno):
                    self.assertTrue(any(keyword.arg is None and isinstance(keyword.value, ast.Call)
                        and isinstance(keyword.value.func, ast.Name)
                        and keyword.value.func.id == 'quiet_process_options' for keyword in call.keywords))

    def test_windows_creates_no_child_console(self):
        with patch.object(quiet_subprocess.os, 'name', 'nt'), patch.object(
                quiet_subprocess.subprocess, 'CREATE_NO_WINDOW', 0x08000000, create=True):
            self.assertEqual(quiet_subprocess.quiet_process_options(), {'creationflags': 0x08000000})

    def test_non_windows_does_not_pass_windows_flags(self):
        with patch.object(quiet_subprocess.os, 'name', 'posix'):
            self.assertEqual(quiet_subprocess.quiet_process_options(), {})

    def test_bounded_inspection_passes_flags_and_keeps_pipes_and_exit_status(self):
        child = Mock(stdout=io.BytesIO(b'packet|dts=1\n'), stderr=io.BytesIO(b'warning\n'))
        child.wait.return_value = 7
        child.poll.return_value = 7
        rows = []
        with patch.object(checker, 'quiet_process_options', return_value={'creationflags': 123}), \
                patch.object(checker.subprocess, 'Popen', return_value=child) as spawn:
            result = checker.bounded_process(['owned-tool', 'unchanged'], rows.append)
        self.assertEqual(spawn.call_args.args, (['owned-tool', 'unchanged'],))
        self.assertEqual(spawn.call_args.kwargs, {'stdin': checker.subprocess.DEVNULL,
            'stdout': checker.subprocess.PIPE, 'stderr': checker.subprocess.PIPE, 'creationflags': 123})
        self.assertEqual(rows, [b'packet|dts=1\n'])
        self.assertEqual(result, (7, 1, True, 'warning\n'))


if __name__ == '__main__':
    unittest.main()
