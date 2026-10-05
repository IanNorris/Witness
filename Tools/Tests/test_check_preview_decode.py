import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("check_preview_decode", Path(__file__).with_name("check_preview_decode.py"))
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class DecodeCheckTests(unittest.TestCase):
    def test_timestamp_faults_are_not_hidden_by_missing_samples(self):
        evidence = CHECK.timestamp_evidence(
            [{"dts": "10"}, {"dts": "10"}, {"dts": "9"}, {"dts": "N/A"}, {}], "dts")
        self.assertEqual(evidence, {"samples": 3, "missing": 2, "duplicates": 1,
                                   "regressions": 1, "minimumStepTicks": -1})

    def test_strict_decode_and_fine_timebase_are_required(self):
        with tempfile.TemporaryDirectory() as root:
            recording = Path(root) / "probe.mp4"
            recording.touch()
            packets = json.dumps({"packets": [{"dts": 0}, {"dts": 4500}]})
            frames = json.dumps({"frames": [{"best_effort_timestamp": 0}, {"best_effort_timestamp": 4500}]})
            for code, stderr, passed in ((0, "", True), (0, "decoder error", False), (1, "", False)):
                with self.subTest(code=code, stderr=stderr), patch.object(CHECK.subprocess, "run", side_effect=[
                    subprocess.CompletedProcess([], code, "", stderr),
                    subprocess.CompletedProcess([], 0, packets, ""),
                    subprocess.CompletedProcess([], 0, frames, ""),
                ]) as run:
                    self.assertEqual(CHECK.inspect(recording)["passed"], passed)
                    command = run.call_args_list[0].args[0]
                    self.assertIn("-xerror", command)
                    self.assertIn("1:1000000", command)
                    self.assertIn("file,pipe", command)
                    self.assertTrue(all(call.kwargs["timeout"] == 120 for call in run.call_args_list))

    def test_missing_or_empty_timestamp_evidence_is_not_a_pass(self):
        self.assertEqual(CHECK.timestamp_evidence([], "dts")["samples"], 0)
        self.assertEqual(CHECK.timestamp_evidence([{}], "dts")["missing"], 1)

    def test_probe_failure_retains_bounded_local_evidence_without_command_line(self):
        with tempfile.TemporaryDirectory() as root:
            recording = Path(root) / 'probe.mp4'
            recording.touch()
            with patch.object(CHECK.subprocess, 'run', side_effect=[
                subprocess.CompletedProcess([], 0, '', ''),
                subprocess.CompletedProcess(['private command'], 7, '', 'probe failure\n' * 10000),
            ]):
                with self.assertRaises(CHECK.RecordingInspectionError) as caught:
                    CHECK.inspect(recording)
            error = caught.exception
            self.assertEqual(error.stage, 'packets')
            self.assertEqual(error.return_code, 7)
            self.assertEqual(len(error.diagnostics), 16384)
            self.assertEqual(str(error), 'timestamp inspection failed')
            self.assertNotIn('private command', str(error))


if __name__ == "__main__":
    unittest.main()
