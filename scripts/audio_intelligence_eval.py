#!/usr/bin/env python3
"""Offline YAMNet evaluation and performance harness for Witness camera clips.

The harness intentionally depends only on numpy, onnxruntime, and FFmpeg tools.
It never writes decoded audio to disk. JSON output contains scores and aggregate
signal/packet statistics, not waveform samples.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
from typing import Any

import numpy as np


SAMPLE_RATE = 16000
WINDOW_SAMPLES = 15360  # 0.96 seconds
HOP_SAMPLES = 7680      # 0.48 seconds

TRIGGER_GROUP_PATTERNS = {
    "speech": ("speech", "conversation", "narration", "whisper", "human voice"),
    "dog": ("dog", "bark", "howl", "growling"),
    "footsteps": ("footstep", "walk", "walking", "run", "running", "shuffle"),
    "vehicle": ("vehicle", "car", "engine", "truck", "motorcycle"),
    "alarm": ("alarm", "siren", "smoke detector"),
    "glass": ("glass", "shatter"),
    "door": ("door", "knock"),
    "wind": ("wind",),
}


def resolve_tool(name: str, explicit: str | None) -> str:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise FileNotFoundError(f"{name} not found: {path}")
        return str(path)
    found = shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(f"{name} was not found on PATH; pass --{name}")


def media_args(path: Path, start: float | None, duration: float | None) -> list[str]:
    args: list[str] = []
    if start is not None:
        args += ["-ss", str(start)]
    args += ["-i", str(path)]
    if duration is not None:
        args += ["-t", str(duration)]
    return args


def decode_audio(ffmpeg: str, path: Path, start: float | None,
                 duration: float | None) -> np.ndarray:
    command = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    command += media_args(path, start, duration)
    command += ["-vn", "-ac", "1", "-ar", str(SAMPLE_RATE),
                "-f", "f32le", "pipe:1"]
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"FFmpeg audio decode failed: {detail}")
    waveform = np.frombuffer(result.stdout, dtype="<f4").copy()
    if waveform.size == 0:
        raise RuntimeError("media contains no decodable audio samples")
    np.clip(waveform, -1.0, 1.0, out=waveform)
    return waveform


def read_packets(ffprobe: str, path: Path, start: float | None,
                 duration: float | None) -> list[dict[str, float | int]]:
    command = [ffprobe, "-v", "error", "-select_streams", "a:0"]
    if start is not None or duration is not None:
        begin = start or 0.0
        interval = f"{begin}%+{duration}" if duration is not None else str(begin)
        command += ["-read_intervals", interval]
    command += ["-show_packets", "-show_entries",
                "packet=pts_time,duration_time,size", "-of", "json", str(path)]
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        return []
    try:
        raw = json.loads(result.stdout).get("packets", [])
    except (UnicodeDecodeError, json.JSONDecodeError):
        return []
    packets: list[dict[str, float | int]] = []
    first_pts: float | None = None
    for packet in raw:
        try:
            pts = float(packet["pts_time"])
            size = int(packet["size"])
            packet_duration = max(0.0, float(packet.get("duration_time", 0.0)))
        except (KeyError, TypeError, ValueError):
            continue
        first_pts = pts if first_pts is None else first_pts
        packets.append({"time": max(0.0, pts - first_pts),
                        "duration": packet_duration, "bytes": size})
    return packets


def load_classes(path: Path) -> list[str]:
    with path.open("r", newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    names = [row["display_name"] for row in rows]
    if len(names) != 521:
        raise RuntimeError(f"expected 521 YAMNet classes, found {len(names)}")
    return names


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    return float(np.percentile(np.asarray(values, dtype=np.float64), fraction * 100.0))


def dbfs(samples: np.ndarray) -> float:
    if samples.size == 0:
        return -120.0
    rms = float(np.sqrt(np.mean(np.square(samples, dtype=np.float64))))
    return max(-120.0, 20.0 * math.log10(max(rms, 1e-6)))


def packet_bytes_for_window(packets: list[dict[str, float | int]],
                            begin: float, end: float) -> int:
    total = 0
    for packet in packets:
        packet_begin = float(packet["time"])
        packet_end = packet_begin + max(float(packet["duration"]), 1e-6)
        if packet_begin < end and packet_end > begin:
            total += int(packet["bytes"])
    return total


def correlation(left: list[float], right: list[float]) -> float | None:
    if len(left) < 2 or len(left) != len(right):
        return None
    if np.std(left) < 1e-9 or np.std(right) < 1e-9:
        return None
    return float(np.corrcoef(left, right)[0, 1])


def top_scores(scores: np.ndarray, classes: list[str], count: int) -> list[dict[str, Any]]:
    indices = np.argsort(scores)[::-1][:count]
    return [{"class": classes[int(index)], "score": float(scores[index])}
            for index in indices]


def trigger_group_scores(scores: np.ndarray, classes: list[str]) -> dict[str, dict[str, float]]:
    lowered = [name.lower() for name in classes]
    groups: dict[str, dict[str, float]] = {}
    for group, patterns in TRIGGER_GROUP_PATTERNS.items():
        indices = [index for index, name in enumerate(lowered)
                   if any(re.search(rf"\b{re.escape(pattern)}\b", name)
                          for pattern in patterns)]
        if not indices:
            continue
        group_scores = np.max(scores[:, indices], axis=1)
        groups[group] = {
            "mean": float(np.mean(group_scores)),
            "peak": float(np.max(group_scores)),
        }
    return groups


def benchmark_windows(session: Any, input_name: str, waveform: np.ndarray,
                      runs: int) -> dict[str, float | int]:
    if runs <= 0:
        return {"runs": 0}
    starts = list(range(0, max(1, waveform.size - WINDOW_SAMPLES + 1), HOP_SAMPLES)) or [0]
    timings: list[float] = []

    def window_for(run: int) -> np.ndarray:
        start = starts[run % len(starts)]
        window = waveform[start:start + WINDOW_SAMPLES]
        if window.size < WINDOW_SAMPLES:
            window = np.pad(window, (0, WINDOW_SAMPLES - window.size))
        return window.astype(np.float32, copy=False)

    # Warm-up separately. Windows process CPU time is fairly coarse, so time
    # the measured batch as a whole instead of rounding every short inference.
    session.run(None, {input_name: window_for(0)})
    cpu_start = time.process_time()
    for run in range(runs):
        wall_start = time.perf_counter()
        session.run(None, {input_name: window_for(run)})
        wall_ms = (time.perf_counter() - wall_start) * 1000.0
        timings.append(wall_ms)
    cpu_mean_ms = (time.process_time() - cpu_start) * 1000.0 / runs
    return {
        "runs": runs,
        "wallMeanMs": statistics.fmean(timings),
        "wallP50Ms": percentile(timings, 0.50),
        "wallP95Ms": percentile(timings, 0.95),
        "cpuMeanMs": cpu_mean_ms,
        # A new overlapping YAMNet frame becomes ready every 0.48 seconds.
        "estimatedCorePercentPerStream": cpu_mean_ms / 480.0 * 100.0,
        "theoreticalStreamsPerCore": 480.0 / max(cpu_mean_ms, 1e-6),
    }


def evaluate(args: argparse.Namespace, media_path: Path, session: Any,
             input_name: str, classes: list[str], ffmpeg: str,
             ffprobe: str | None) -> dict[str, Any]:
    decode_start = time.perf_counter()
    waveform = decode_audio(ffmpeg, media_path, args.start, args.duration)
    decode_ms = (time.perf_counter() - decode_start) * 1000.0
    media_duration = waveform.size / SAMPLE_RATE
    packets = read_packets(ffprobe, media_path, args.start, args.duration) if ffprobe else []

    inference_start = time.perf_counter()
    cpu_start = time.process_time()
    outputs = session.run(None, {input_name: waveform})
    inference_cpu_ms = (time.process_time() - cpu_start) * 1000.0
    inference_ms = (time.perf_counter() - inference_start) * 1000.0
    scores = np.asarray(outputs[0])
    if scores.ndim != 2 or scores.shape[1] != len(classes):
        raise RuntimeError(f"unexpected YAMNet score shape: {scores.shape}")

    silence_indices = [index for index, name in enumerate(classes)
                       if name.lower() == "silence"]
    windows: list[dict[str, Any]] = []
    packet_sizes: list[float] = []
    levels: list[float] = []
    non_silence_scores: list[float] = []
    for index, frame_scores in enumerate(scores):
        begin = index * HOP_SAMPLES / SAMPLE_RATE
        end = begin + WINDOW_SAMPLES / SAMPLE_RATE
        sample_begin = index * HOP_SAMPLES
        samples = waveform[sample_begin:sample_begin + WINDOW_SAMPLES]
        level = dbfs(samples)
        packet_bytes = packet_bytes_for_window(packets, begin, end) if packets else 0
        non_silence = float(np.max(np.delete(frame_scores, silence_indices))) \
            if silence_indices else float(np.max(frame_scores))
        window = {
            "startSeconds": begin,
            "endSeconds": min(end, media_duration),
            "rmsDbfs": level,
            "packetBytes": packet_bytes if packets else None,
            "nonSilenceScore": non_silence,
            "top": top_scores(frame_scores, classes, args.window_top),
        }
        windows.append(window)
        if packets:
            packet_sizes.append(float(packet_bytes))
            levels.append(level)
            non_silence_scores.append(non_silence)

    mean_scores = np.mean(scores, axis=0)
    max_scores = np.max(scores, axis=0)
    packet_analysis = {
        "available": bool(packets),
        "packetCount": len(packets),
        "bytesVsRmsCorrelation": correlation(packet_sizes, levels),
        "bytesVsNonSilenceScoreCorrelation": correlation(packet_sizes, non_silence_scores),
    }
    return {
        "media": str(media_path.resolve()),
        "durationSeconds": media_duration,
        "sampleRate": SAMPLE_RATE,
        "samples": int(waveform.size),
        "frames": int(scores.shape[0]),
        "timing": {
            "decodeWallMs": decode_ms,
            "inferenceWallMs": inference_ms,
            "inferenceCpuMs": inference_cpu_ms,
            "inferenceRealtimeFactor": inference_ms / max(media_duration * 1000.0, 1e-6),
            "windowBenchmark": benchmark_windows(session, input_name, waveform,
                                                   args.benchmark_runs),
        },
        "signal": {
            "wholeClipRmsDbfs": dbfs(waveform),
            "peakDbfs": 20.0 * math.log10(max(float(np.max(np.abs(waveform))), 1e-6)),
        },
        "topMean": top_scores(mean_scores, classes, args.top),
        "topPeak": top_scores(max_scores, classes, args.top),
        "triggerGroups": trigger_group_scores(scores, classes),
        "packetGate": packet_analysis,
        "windows": windows,
    }


def print_report(report: dict[str, Any]) -> None:
    timing = report["timing"]
    benchmark = timing["windowBenchmark"]
    print(f"\n{report['media']}")
    print(f"  audio {report['durationSeconds']:.2f}s, {report['frames']} YAMNet frames, "
          f"RMS {report['signal']['wholeClipRmsDbfs']:.1f} dBFS")
    print(f"  whole-clip inference {timing['inferenceWallMs']:.1f} ms wall / "
          f"{timing['inferenceCpuMs']:.1f} ms CPU "
          f"(RTF {timing['inferenceRealtimeFactor']:.4f})")
    if benchmark.get("runs"):
        print(f"  0.96s window CPU {benchmark['cpuMeanMs']:.1f} ms mean; "
              f"wall p50/p95 {benchmark['wallP50Ms']:.1f}/{benchmark['wallP95Ms']:.1f} ms; "
              f"~{benchmark['estimatedCorePercentPerStream']:.2f}% core/stream")
    print("  top mean scores:")
    for item in report["topMean"]:
        print(f"    {item['score']:.3f}  {item['class']}")
    strongest_groups = sorted(report["triggerGroups"].items(),
                              key=lambda item: item[1]["peak"], reverse=True)[:5]
    print("  trigger-group peaks: " + ", ".join(
        f"{name}={scores['peak']:.3f}" for name, scores in strongest_groups))
    packet = report["packetGate"]
    if packet["available"]:
        print(f"  packet-size correlation: RMS={packet['bytesVsRmsCorrelation']}, "
              f"non-silence={packet['bytesVsNonSilenceScoreCorrelation']}")
    else:
        print("  packet-size correlation unavailable (no usable audio packet timestamps)")


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media", nargs="+", type=Path)
    parser.add_argument("--model", type=Path, default=repo / "models" / "yamnet.onnx")
    parser.add_argument("--classes", type=Path,
                        default=repo / "models" / "yamnet_class_map.csv")
    parser.add_argument("--ffmpeg")
    parser.add_argument("--ffprobe")
    parser.add_argument("--start", type=float)
    parser.add_argument("--duration", type=float)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--window-top", type=int, default=3)
    parser.add_argument("--benchmark-runs", type=int, default=50)
    parser.add_argument("--threads", type=int, default=1,
                        help="ONNX intra/inter-op CPU threads (default: 1 for capacity measurements)")
    parser.add_argument("--json", type=Path, help="write the complete machine-readable report")
    args = parser.parse_args()

    for path in [args.model, args.classes, *args.media]:
        if not path.is_file():
            parser.error(f"file not found: {path}")
    if args.top < 1 or args.window_top < 1 or args.benchmark_runs < 0 or args.threads < 1:
        parser.error("top counts and threads must be positive; benchmark runs cannot be negative")

    try:
        import onnxruntime as ort
        ffmpeg = resolve_tool("ffmpeg", args.ffmpeg)
        try:
            ffprobe = resolve_tool("ffprobe", args.ffprobe)
        except FileNotFoundError:
            ffprobe = None
        classes = load_classes(args.classes)
        options = ort.SessionOptions()
        options.intra_op_num_threads = args.threads
        options.inter_op_num_threads = args.threads
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        load_start = time.perf_counter()
        session = ort.InferenceSession(str(args.model), sess_options=options,
                                       providers=["CPUExecutionProvider"])
        model_load_ms = (time.perf_counter() - load_start) * 1000.0
        input_name = session.get_inputs()[0].name
        reports = [evaluate(args, path, session, input_name, classes, ffmpeg, ffprobe)
                   for path in args.media]
        for report in reports:
            report["modelLoadWallMs"] = model_load_ms
            print_report(report)
        output = {
            "schema": 1,
            "model": str(args.model.resolve()),
            "modelSha256": sha256_file(args.model),
            "classMapSha256": sha256_file(args.classes),
            "provider": "CPUExecutionProvider",
            "threads": args.threads,
            "reports": reports,
        }
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(output, indent=2), encoding="utf-8")
            print(f"\nWrote {args.json}")
        return 0
    except Exception as exc:
        print(f"audio evaluation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
