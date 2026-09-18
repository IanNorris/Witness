#!/usr/bin/env python3
"""Evaluate cheap, explainable activity similarity on Witness images.

This is an offline research tool, not a production grouping implementation.  It
can group clip thumbnails using camera-local bounded comparisons, or compare
two steady-state frames to distinguish local changes from a whole-scene reset.
Only compact descriptors and aggregate measurements are written to JSON.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import cv2
import numpy as np


CLIP_NAME = re.compile(r"^(?P<camera>\d+)_(?P<timestamp>\d+)(?:\.\d+)?\.jpg$", re.I)


def _bits_to_hex(bits: np.ndarray) -> str:
    packed = np.packbits(bits.astype(np.uint8).reshape(-1))
    return packed.tobytes().hex()


def _hex_hamming(left: str, right: str) -> int:
    return (int(left, 16) ^ int(right, 16)).bit_count()


def _dhash(gray: np.ndarray) -> str:
    small = cv2.resize(gray, (9, 8), interpolation=cv2.INTER_AREA)
    return _bits_to_hex(small[:, 1:] > small[:, :-1])


def _phash(gray: np.ndarray) -> str:
    small = cv2.resize(gray, (32, 32), interpolation=cv2.INTER_AREA).astype(np.float32)
    low = cv2.dct(small)[:8, :8]
    values = low.reshape(-1)
    threshold = float(np.median(values[1:]))
    bits = values > threshold
    bits[0] = False
    return _bits_to_hex(bits)


def _block_hash(gray: np.ndarray) -> str:
    blocks = cv2.resize(gray, (16, 16), interpolation=cv2.INTER_AREA)
    return _bits_to_hex(blocks > np.median(blocks))


def _normalised_grid(gray: np.ndarray) -> np.ndarray:
    grid = cv2.resize(gray, (8, 8), interpolation=cv2.INTER_AREA).astype(np.float32)
    mean = float(grid.mean())
    std = max(float(grid.std()), 8.0)
    return np.clip((grid - mean) / std, -3.0, 3.0)


def _hsv_histogram(image: np.ndarray) -> np.ndarray:
    hsv = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)
    hist = cv2.calcHist([hsv], [0, 1], None, [12, 4], [0, 180, 0, 256])
    return cv2.normalize(hist, hist).reshape(-1)


@dataclass
class Descriptor:
    path: Path
    camera: int
    timestamp: int
    width: int
    height: int
    mean_luma: float
    colourfulness: float
    dhash: str
    phash: str
    block_hash: str
    grid: np.ndarray
    histogram: np.ndarray

    @property
    def regime(self) -> str:
        # A coarse compatibility gate, not a claim that the scene is day/night.
        if self.mean_luma < 35:
            return "dark"
        if self.colourfulness < 10:
            return "monochrome"
        return "colour"


def describe(path: Path) -> Descriptor:
    match = CLIP_NAME.match(path.name)
    if not match:
        raise ValueError(f"not a Witness clip thumbnail name: {path.name}")
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None or image.size == 0:
        raise ValueError("image could not be decoded")
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    lab = cv2.cvtColor(image, cv2.COLOR_BGR2LAB)
    colourfulness = float(np.mean(np.abs(lab[:, :, 1:].astype(np.float32) - 128.0)))
    return Descriptor(
        path=path,
        camera=int(match.group("camera")),
        timestamp=int(match.group("timestamp")),
        width=int(image.shape[1]),
        height=int(image.shape[0]),
        mean_luma=float(gray.mean()),
        colourfulness=colourfulness,
        dhash=_dhash(gray),
        phash=_phash(gray),
        block_hash=_block_hash(gray),
        grid=_normalised_grid(gray),
        histogram=_hsv_histogram(image),
    )


def distance(left: Descriptor, right: Descriptor) -> dict[str, float]:
    dhash = _hex_hamming(left.dhash, right.dhash) / 64.0
    phash = _hex_hamming(left.phash, right.phash) / 64.0
    block = _hex_hamming(left.block_hash, right.block_hash) / 256.0
    grid = float(np.mean(np.abs(left.grid - right.grid))) / 3.0
    histogram = float(cv2.compareHist(left.histogram, right.histogram, cv2.HISTCMP_BHATTACHARYYA))
    # pHash carries most weight; the other terms make illumination-only and
    # spatially different matches less attractive while remaining explainable.
    combined = 0.42 * phash + 0.16 * dhash + 0.16 * block + 0.16 * grid + 0.10 * histogram
    return {
        "combined": combined,
        "phash": phash,
        "dhash": dhash,
        "block": block,
        "grid": grid,
        "histogram": histogram,
    }


def discover(paths: Iterable[str], limit: int) -> list[Path]:
    result: list[Path] = []
    for raw in paths:
        path = Path(raw)
        candidates = path.glob("*.jpg") if path.is_dir() else (path,)
        for candidate in candidates:
            if candidate.is_file() and candidate.stat().st_size and CLIP_NAME.match(candidate.name):
                result.append(candidate)
    # Limit by recency across the installation, then restore chronological
    # order. Sorting by camera first would accidentally bias a limited sample
    # toward the highest camera IDs.
    result.sort(key=lambda item: int(CLIP_NAME.match(item.name).group("timestamp")))
    return result[-limit:] if limit and len(result) > limit else result


def group_images(args: argparse.Namespace) -> dict[str, Any]:
    started = time.perf_counter()
    files = discover(args.paths, args.limit)
    descriptors: list[Descriptor] = []
    failures: list[dict[str, str]] = []
    descriptor_times: list[float] = []
    for path in files:
        before = time.perf_counter()
        try:
            descriptors.append(describe(path))
        except (OSError, ValueError, cv2.error) as error:
            failures.append({"path": str(path), "error": str(error)})
        descriptor_times.append((time.perf_counter() - before) * 1000.0)

    groups: list[dict[str, Any]] = []
    comparisons = 0
    for descriptor in descriptors:
        best: tuple[float, dict[str, float], dict[str, Any]] | None = None
        # Candidates are intentionally bounded to recent group representatives.
        candidates = [group for group in reversed(groups)
                      if group["camera"] == descriptor.camera
                      and descriptor.timestamp - group["last_timestamp"] <= args.max_gap
                      and descriptor.timestamp - group["first_timestamp"] <= args.max_span
                      and group["count"] < args.max_group_size]
        for group in candidates[:args.max_candidates]:
            representative: Descriptor = group["representative_descriptor"]
            if representative.regime != descriptor.regime:
                continue
            metrics = distance(representative, descriptor)
            comparisons += 1
            if best is None or metrics["combined"] < best[0]:
                best = (metrics["combined"], metrics, group)
        if best is not None and best[0] <= args.threshold:
            _, metrics, group = best
            group["members"].append({"path": str(descriptor.path), "timestamp": descriptor.timestamp,
                                     "distance": metrics})
            group["count"] += 1
            group["last_timestamp"] = descriptor.timestamp
        else:
            groups.append({
                "camera": descriptor.camera,
                "regime": descriptor.regime,
                "first_timestamp": descriptor.timestamp,
                "last_timestamp": descriptor.timestamp,
                "count": 1,
                "representative": str(descriptor.path),
                "representative_descriptor": descriptor,
                "members": [{"path": str(descriptor.path), "timestamp": descriptor.timestamp,
                             "distance": None}],
            })

    serialised_groups = []
    for group in groups:
        serialised_groups.append({key: value for key, value in group.items()
                                  if key != "representative_descriptor"})
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    grouped_count = sum(max(0, group["count"] - 1) for group in groups)
    return {
        "mode": "group",
        "parameters": {
            "threshold": args.threshold,
            "max_gap_seconds": args.max_gap,
            "max_span_seconds": args.max_span,
            "max_group_size": args.max_group_size,
            "max_candidates": args.max_candidates,
        },
        "summary": {
            "files_found": len(files),
            "images_described": len(descriptors),
            "failures": len(failures),
            "groups": len(groups),
            "clips_joined_to_existing_group": grouped_count,
            "card_reduction_percent": (100.0 * grouped_count / len(descriptors)) if descriptors else 0.0,
            "comparisons": comparisons,
            "elapsed_ms": elapsed_ms,
            "descriptor_mean_ms": statistics.fmean(descriptor_times) if descriptor_times else 0.0,
            "descriptor_p95_ms": percentile(descriptor_times, 95),
        },
        "groups": serialised_groups,
        "failures": failures,
    }


def percentile(values: list[float], percent: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(percent / 100.0 * len(ordered)) - 1))
    return ordered[index]


def compare_baselines(args: argparse.Namespace) -> dict[str, Any]:
    before = cv2.imread(str(args.before), cv2.IMREAD_COLOR)
    after = cv2.imread(str(args.after), cv2.IMREAD_COLOR)
    if before is None or after is None:
        raise SystemExit("both baseline images must decode successfully")
    if before.shape[:2] != after.shape[:2]:
        after = cv2.resize(after, (before.shape[1], before.shape[0]), interpolation=cv2.INTER_AREA)

    # CLAHE reduces sensitivity to broad exposure changes. Median blur suppresses
    # sensor/JPEG noise; morphology turns stable changed pixels into regions.
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    left = clahe.apply(cv2.cvtColor(before, cv2.COLOR_BGR2GRAY))
    right = clahe.apply(cv2.cvtColor(after, cv2.COLOR_BGR2GRAY))
    left = cv2.medianBlur(left, 5)
    right = cv2.medianBlur(right, 5)
    difference = cv2.absdiff(left, right)
    _, mask = cv2.threshold(difference, args.pixel_threshold, 255, cv2.THRESH_BINARY)
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 5))
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=2)
    changed_fraction = float(np.count_nonzero(mask)) / float(mask.size)
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    minimum_area = mask.size * args.minimum_region_fraction
    regions = []
    for contour in contours:
        area = float(cv2.contourArea(contour))
        if area < minimum_area:
            continue
        x, y, width, height = cv2.boundingRect(contour)
        regions.append({
            "x": x / mask.shape[1], "y": y / mask.shape[0],
            "width": width / mask.shape[1], "height": height / mask.shape[0],
            "changed_area_fraction": area / mask.size,
        })
    regions.sort(key=lambda item: item["changed_area_fraction"], reverse=True)
    histogram_left = cv2.calcHist([left], [0], None, [32], [0, 256])
    histogram_right = cv2.calcHist([right], [0], None, [32], [0, 256])
    histogram_distance = float(cv2.compareHist(
        cv2.normalize(histogram_left, histogram_left),
        cv2.normalize(histogram_right, histogram_right), cv2.HISTCMP_BHATTACHARYYA))
    reset = changed_fraction >= args.reset_fraction or histogram_distance >= args.reset_histogram_distance
    return {
        "mode": "compare",
        "before": str(args.before),
        "after": str(args.after),
        "changed_pixel_fraction": changed_fraction,
        "luma_histogram_distance": histogram_distance,
        "whole_scene_reset": reset,
        "decision": "establish-new-baseline-and-transfer-regions" if reset else "local-region-change",
        "regions": [] if reset else regions,
        "parameters": {
            "pixel_threshold": args.pixel_threshold,
            "minimum_region_fraction": args.minimum_region_fraction,
            "reset_fraction": args.reset_fraction,
            "reset_histogram_distance": args.reset_histogram_distance,
        },
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="write the full JSON report here")
    subparsers = parser.add_subparsers(dest="mode", required=True)

    group = subparsers.add_parser("group", help="group Witness clip thumbnails")
    group.add_argument("paths", nargs="+", help="thumbnail files or directories")
    group.add_argument("--limit", type=int, default=2000)
    group.add_argument("--threshold", type=float, default=0.20)
    group.add_argument("--max-gap", type=int, default=60)
    group.add_argument("--max-span", type=int, default=600)
    group.add_argument("--max-group-size", type=int, default=50)
    group.add_argument("--max-candidates", type=int, default=8)
    group.set_defaults(handler=group_images)

    compare = subparsers.add_parser("compare", help="compare two steady-state scene frames")
    compare.add_argument("before", type=Path)
    compare.add_argument("after", type=Path)
    compare.add_argument("--pixel-threshold", type=int, default=28)
    compare.add_argument("--minimum-region-fraction", type=float, default=0.001)
    compare.add_argument("--reset-fraction", type=float, default=0.45)
    compare.add_argument("--reset-histogram-distance", type=float, default=0.55)
    compare.set_defaults(handler=compare_baselines)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    report = args.handler(args)
    encoded = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    summary = report.get("summary", report)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
