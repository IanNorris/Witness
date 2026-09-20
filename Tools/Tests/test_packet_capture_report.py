import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "packet_capture_report.py"


class PacketCaptureReportTests(unittest.TestCase):
    def test_correlates_packets_and_replays_independent_partial(self):
        with tempfile.TemporaryDirectory(prefix="witness-packet-report-") as temp:
            directory = Path(temp)
            (directory / "input.bin").write_bytes(b"input")
            (directory / "output.bin").write_bytes(b"initoldgood")
            rows = [
                {"type": "input", "activityId": 1, "audio": False,
                 "hashFnv64": "bf0c269a1785ba74", "offset": 0, "bytes": 5},
                {"type": "decision", "activityId": 1, "audio": False,
                 "hashFnv64": "bf0c269a1785ba74", "disposition": "written"},
                {"type": "init", "generation": 0, "offset": 0, "bytes": 4},
                {"type": "partial", "generation": 0, "independent": False,
                 "offset": 4, "bytes": 3},
                {"type": "partial", "generation": 0, "independent": True,
                 "offset": 7, "bytes": 4},
                {"type": "summary", "queueRejectedRecords": 0},
            ]
            (directory / "events.jsonl").write_text(
                "".join(json.dumps(row) + "\n" for row in rows))
            replay = directory / "replay.mp4"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(directory), "--replay", str(replay)],
                capture_output=True, text=True, check=True)
            self.assertIn("Missing source correlation: 0", result.stdout)
            self.assertIn("payload changes before mux: 0", result.stdout)
            self.assertEqual(replay.read_bytes(), b"initgood")


if __name__ == "__main__":
    unittest.main()
