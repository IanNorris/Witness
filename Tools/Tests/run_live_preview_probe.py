"""Run the opt-in camera probe without exposing stream credentials in argv/logs."""
import argparse
from contextlib import closing
import os
import re
from pathlib import Path
import sqlite3
import subprocess
import sys
import threading


def safe_report(line):
    """Accept numeric reports, never arbitrary text merely sharing a prefix."""
    value = line.rstrip("\r\n")
    if value.startswith(("elapsedSeconds=", "readResult=", "probePauseSeconds=", "initializationResult=")):
        if re.fullmatch(r"[A-Za-z][A-Za-z0-9]*=[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"
                        r"(?: [A-Za-z][A-Za-z0-9]*=[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)*", value):
            return value
    elif value.startswith("captureDirectory="):
        # The native probe's fixed relative capture root and numeric IDs only.
        if re.fullmatch(r"captureDirectory=build-vs2026[/\\]packet-captures[/\\]camera-\d+-preview-\d+-\d+", value):
            return value
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--camera", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=900)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capture-delay", type=int,
                        help="also capture 30 seconds of input/output packets after this delay")
    parser.add_argument("--capture-every", type=int, default=31,
                        help="seconds between capture starts, at least 31")
    parser.add_argument("--capture-count", type=int, default=1,
                        help="number of bounded 30-second captures, 1..8")
    parser.add_argument("--pause-at", type=int, help="inject one local probe-processing pause after this many seconds")
    parser.add_argument("--pause-for", type=int, help="pause length, 1..15 seconds; requires --pause-at")
    args = parser.parse_args()
    if not 10 <= args.seconds <= 1800 or args.camera <= 0:
        parser.error("seconds must be 10..1800 and camera must be positive")
    if args.capture_delay is not None and not 0 <= args.capture_delay <= args.seconds - 30:
        parser.error("capture delay must leave at least 30 seconds in the test")
    if not 1 <= args.capture_count <= 8 or args.capture_every < 31:
        parser.error("capture count must be 1..8 and interval at least 31 seconds")
    if args.capture_count != 1 and args.capture_delay is None:
        parser.error("repeated captures require capture-delay")
    if args.capture_delay is not None and (args.capture_delay +
            args.capture_every * (args.capture_count - 1) + 30 > args.seconds):
        parser.error("all capture windows must fit within the test")
    if (args.pause_at is None) != (args.pause_for is None):
        parser.error("pause-at and pause-for must be supplied together")
    if args.pause_at is not None and (args.pause_at < 0 or not 1 <= args.pause_for <= 15 or
                                      args.pause_at + args.pause_for + 5 > args.seconds):
        parser.error("pause must be bounded to 15 seconds and leave time for recovery")
    output = args.output.resolve()
    if output.exists() or list(output.parent.glob(output.name + ".generation-*.mp4")):
        parser.error("output or generation files already exist; choose a fresh prefix")
    with closing(sqlite3.connect(args.database.resolve().as_uri() + "?mode=ro", uri=True)) as db:
        row = db.execute("SELECT CameraStringSub FROM Camera WHERE CameraUID=?", (args.camera,)).fetchone()
    if not row or not row[0]:
        parser.error("camera has no configured preview URL")
    env = os.environ.copy()
    env["WITNESS_PREVIEW_PROBE_URL"] = row[0]
    # The wrapper tests a preview, not the optional Reolink/audio profile.
    env.pop("WITNESS_PROBE_REOLINK", None)
    env.pop("WITNESS_PROBE_CAPTURE", None)
    env.pop("WITNESS_PROBE_CAPTURE_DELAY", None)
    env.pop("WITNESS_PROBE_CAPTURE_EVERY", None)
    env.pop("WITNESS_PROBE_CAPTURE_COUNT", None)
    env.pop("WITNESS_PROBE_PAUSE_AT", None)
    env.pop("WITNESS_PROBE_PAUSE_SECONDS", None)
    if args.capture_delay is not None:
        env["WITNESS_PROBE_CAPTURE"] = "1"
        env["WITNESS_PROBE_CAPTURE_DELAY"] = str(args.capture_delay)
        env["WITNESS_PROBE_CAPTURE_EVERY"] = str(args.capture_every)
        env["WITNESS_PROBE_CAPTURE_COUNT"] = str(args.capture_count)
    if args.pause_at is not None:
        env["WITNESS_PROBE_PAUSE_AT"] = str(args.pause_at)
        env["WITNESS_PROBE_PAUSE_SECONDS"] = str(args.pause_for)
    with subprocess.Popen([str(args.probe.resolve()), str(args.seconds), str(output), str(args.camera)],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, errors="replace") as process:
        # Independent bound also covers initialization and a stuck native read.
        deadline = threading.Timer(args.seconds + 60, process.kill)
        deadline.daemon = True
        deadline.start()
        try:
            for line in process.stdout:
                # Never relay arbitrary native/FFmpeg logs, which may contain URLs.
                # Emit only numeric probe reports and the bounded local capture path.
                report = safe_report(line)
                if report is not None:
                    print(report, flush=True)
            result = process.wait(timeout=60)
        except BaseException:
            process.kill()
            process.wait()
            raise
        finally:
            deadline.cancel()
    print(f"probeExit={result}", flush=True)
    return result


if __name__ == "__main__":
    sys.exit(main())
