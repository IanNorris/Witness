"""Strict, local-only decode and timestamp checks for bounded probe recordings."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


class RecordingInspectionError(ValueError):
    """Bounded local failure details; the command-line interface stays generic."""
    def __init__(self, stage, return_code, diagnostics):
        super().__init__('timestamp inspection failed')
        self.stage = stage
        self.return_code = return_code
        self.diagnostics = diagnostics[:16384]


def timestamp_evidence(rows, field):
    values = [int(row[field]) for row in rows if row.get(field) not in (None, "N/A")]
    steps = [right - left for left, right in zip(values, values[1:])]
    return {
        "samples": len(values),
        "missing": len(rows) - len(values),
        "duplicates": sum(step == 0 for step in steps),
        "regressions": sum(step < 0 for step in steps),
        "minimumStepTicks": min(steps) if steps else None,
    }


def inspect(path, ffmpeg="ffmpeg", ffprobe="ffprobe"):
    path = Path(path).resolve()
    if not path.is_file() or path.stat().st_size > 512 * 1024 * 1024:
        raise ValueError("expected a local probe recording no larger than 512 MiB")
    # Never allow a media file to make network requests through its demuxer.
    common = ["-v", "error", "-protocol_whitelist", "file,pipe"]
    decoded = subprocess.run(
        [ffmpeg, *common, "-xerror", "-i", str(path), "-map", "0:v:0",
         "-enc_time_base", "1:1000000", "-fps_mode", "passthrough", "-f", "null", "-"],
        capture_output=True, text=True, timeout=120,
    )
    report = {"file": str(path), "strictDecodeExit": decoded.returncode,
              "decodeDiagnosticLines": len(decoded.stderr.splitlines())}
    for section, field in (("packets", "dts"), ("frames", "best_effort_timestamp")):
        result = subprocess.run(
            [ffprobe, *common, "-select_streams", "v:0", "-show_" + section,
             "-show_entries", section[:-1] + "=" + field, "-of", "json", str(path)],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode:
            raise RecordingInspectionError(section, result.returncode, result.stderr)
        rows = json.loads(result.stdout).get(section, [])
        report[section] = timestamp_evidence(rows, field)
        report[section]["diagnosticLines"] = len(result.stderr.splitlines())
    report["passed"] = (decoded.returncode == 0 and not decoded.stderr.strip() and
                        all(report[section]["samples"] > 1 and
                            report[section]["missing"] == 0 and
                            report[section]["duplicates"] == 0 and
                            report[section]["regressions"] == 0 and
                            report[section]["diagnosticLines"] == 0
                            for section in ("packets", "frames")))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    try:
        report = inspect(args.recording, args.ffmpeg, args.ffprobe)
    except (ValueError, OSError, subprocess.TimeoutExpired):
        # Do not relay arbitrary decoder logs or exception command lines.
        print(json.dumps({"passed": False, "inspectionFailed": True}))
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
