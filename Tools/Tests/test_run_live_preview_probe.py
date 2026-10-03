import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import MagicMock, patch

SCRIPT = Path(__file__).with_name("run_live_preview_probe.py")
SPEC = importlib.util.spec_from_file_location("run_live_preview_probe", SCRIPT)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class ProbeRunnerTests(unittest.TestCase):
    def test_pause_requires_bounded_complete_pair(self):
        base = [str(SCRIPT), "--database", "unused.db", "--probe", "unused.exe",
                "--camera", "9", "--seconds", "120", "--output", "unused.mp4"]
        for extra in (["--pause-at", "60"], ["--pause-for", "10"],
                      ["--pause-at", "60", "--pause-for", "16"],
                      ["--pause-at", "119", "--pause-for", "1"]):
            with self.subTest(extra=extra), patch("sys.argv", base + extra), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                RUNNER.main()
            self.assertEqual(caught.exception.code, 2)

    def test_credentials_are_environment_only_and_logs_are_filtered(self):
        with tempfile.TemporaryDirectory(prefix="witness-probe-test-") as root:
            root = Path(root)
            database = root / "camera.db"
            secret_url = "rtsp://example:secret@camera.invalid/preview"
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.execute("CREATE TABLE Camera(CameraUID INTEGER, CameraStringSub TEXT)")
                db.execute("INSERT INTO Camera VALUES(9,?)", (secret_url,))
                db.commit()
            child = MagicMock()
            child.__enter__.return_value = child
            child.stdout = iter([f"native log {secret_url}\n", "elapsedSeconds=60 accepted=1140\n"])
            child.wait.return_value = 0
            argv = [str(SCRIPT), "--database", str(database), "--probe", str(root / "probe.exe"),
                    "--camera", "9", "--seconds", "120", "--output", str(root / "new.mp4")]
            output = io.StringIO()
            with patch("sys.argv", argv), patch.object(RUNNER.subprocess, "Popen", return_value=child) as popen, \
                    patch.object(RUNNER.threading, "Timer") as timer, \
                    patch.dict(os.environ, {"WITNESS_PROBE_REOLINK": "1", "WITNESS_PROBE_CAPTURE": "1"}), \
                    contextlib.redirect_stdout(output):
                self.assertEqual(RUNNER.main(), 0)
            self.assertNotIn(secret_url, str(popen.call_args.args[0]))
            self.assertEqual(popen.call_args.kwargs["env"]["WITNESS_PREVIEW_PROBE_URL"], secret_url)
            self.assertNotIn("WITNESS_PROBE_REOLINK", popen.call_args.kwargs["env"])
            self.assertNotIn("WITNESS_PROBE_CAPTURE", popen.call_args.kwargs["env"])
            self.assertNotIn("secret", output.getvalue())
            self.assertIn("elapsedSeconds=60 accepted=1140", output.getvalue())
            self.assertEqual(timer.call_args.args[0], 180)
            timer.return_value.start.assert_called_once()
            timer.return_value.cancel.assert_called_once()


if __name__ == "__main__":
    unittest.main()
