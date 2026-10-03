import json
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "packet_capture_report.py"
SPEC = importlib.util.spec_from_file_location("packet_capture_report", SCRIPT)
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class PacketCaptureReportTests(unittest.TestCase):
    def test_input_replay_preserves_access_units_and_starts_at_keyframe(self):
        extra, old, key, following = [b"\x00\x00\x01" + bytes([value]) for value in (7, 1, 5, 1)]
        blob = extra + old + key + following
        rows = [{"type": "stream", "streamIndex": 0, "audio": False, "codecId": 27},
                {"type": "extradata", "streamIndex": 0, "offset": 0, "bytes": 4}]
        rows += [{"type": "input", "audio": False, "streamIndex": 0, "activityId": index,
                  "offset": index * 4, "bytes": 4, "flags": int(index == 2)}
                 for index in range(1, 4)]
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp) / "input.h264"
            self.assertEqual(REPORT.replay_input_video(rows, blob, destination), (2, 2))
            self.assertEqual(destination.read_bytes(), extra + key + following)
            with self.assertRaises(FileExistsError):
                REPORT.replay_input_video(rows, blob, destination)
            rows[0]["codecId"] = 173
            self.assertEqual(REPORT.replay_input_video(rows, blob, Path(temp) / "input.hevc"), (2, 2))
            rows[1]["offset"] = 1
            with self.assertRaisesRegex(ValueError, "Annex-B"):
                REPORT.replay_input_video(rows, blob, Path(temp) / "invalid.h264")
            self.assertFalse((Path(temp) / "invalid.h264").exists())
            rows[1]["offset"] = 0
            for row in rows[2:]:
                row["flags"] = 0
            with self.assertRaisesRegex(ValueError, "keyframe"):
                REPORT.replay_input_video(rows, blob, Path(temp) / "no-key.hevc")
            rows.append({"type": "stream", "audio": False, "codecId": 27, "streamIndex": 1})
            with self.assertRaisesRegex(ValueError, "exactly one"):
                REPORT.replay_input_video(rows, blob, Path(temp) / "ambiguous.h264")

    def test_observed_delivery_ignores_regressing_source_dts(self):
        rows = [{"type": "input", "audio": False, "activityId": frame,
                 "observedSteadyUs": 1000000 + frame * 53000,
                 "dts": 90000 if frame % 2 else 0} for frame in range(20)]
        rows.append({"type": "decision", "audio": False, "activityId": 5,
                     "muxObservedSteadyUs": 1000000 + 6 * 53000,
                     "decisionSteadyUs": 1000000 + 6 * 53000 + 500})
        result = REPORT.delivery_timing(rows)
        self.assertTrue(result["available"])
        self.assertAlmostEqual(result["deliveredFps"], 1000 / 53)
        self.assertEqual(result["arrivalGaps"]["p99Ms"], 53)
        self.assertEqual(result["demuxToMuxIncludingLookahead"]["p50Ms"], 53)
        self.assertEqual(result["muxToDecision"]["p50Ms"], .5)

    def test_missing_or_backward_observations_do_not_invent_timing(self):
        rows = [{"type": "input", "audio": False, "activityId": frame,
                 "observedSteadyUs": 100 - frame} for frame in range(3)]
        self.assertFalse(REPORT.delivery_timing(rows)["available"])
        rows[0].pop("observedSteadyUs")
        self.assertFalse(REPORT.delivery_timing(rows)["available"])

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
