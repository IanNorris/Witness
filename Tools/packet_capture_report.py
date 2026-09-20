#!/usr/bin/env python3
"""Inspect a bounded Witness packet capture, optionally reconstruct its fMP4."""

import argparse
import collections
import json
from pathlib import Path


def fnv64(data: bytes) -> str:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def payload(blob: bytes, event: dict) -> bytes:
    start = event["offset"]
    end = start + event["bytes"]
    if start < 0 or end > len(blob):
        raise ValueError(f"out-of-range {event['type']} payload at {start}:{end}")
    return blob[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("--replay", type=Path, help="write a playable output fMP4")
    args = parser.parse_args()

    events = [json.loads(line) for line in
              (args.capture_dir / "events.jsonl").read_text().splitlines()]
    raw_input = (args.capture_dir / "input.bin").read_bytes()
    raw_output = (args.capture_dir / "output.bin").read_bytes()
    inputs = {}
    decisions = []
    outputs = []
    bad_hashes = []
    for event in events:
        kind = event["type"]
        if kind == "input":
            inputs[(event["activityId"], event["audio"])] = event
            if fnv64(payload(raw_input, event)) != event["hashFnv64"]:
                bad_hashes.append((kind, event["activityId"]))
        elif kind == "decision":
            decisions.append(event)
        elif kind in ("init", "partial"):
            outputs.append(event)
            if "hashFnv64" in event and fnv64(payload(raw_output, event)) != event["hashFnv64"]:
                bad_hashes.append((kind, event.get("segment")))

    dispositions = collections.Counter(item["disposition"] for item in decisions)
    missing = [item for item in decisions if
               (item["activityId"], item["audio"]) not in inputs]
    changed = [item for item in decisions if
               (source := inputs.get((item["activityId"], item["audio"]))) is not None
               and source["hashFnv64"] != item["hashFnv64"]]
    print(f"Input packets: {len(inputs)}, live-mux decisions: {len(decisions)}")
    print(f"Output chunks: {len(outputs)}, input bytes: {len(raw_input)}, output bytes: {len(raw_output)}")
    print(f"Disposition counts: {dict(dispositions)}")
    print(f"Missing source correlation: {len(missing)}, payload changes before mux: {len(changed)}")
    print(f"Binary hash mismatches: {len(bad_hashes)}")
    summaries = [event for event in events if event["type"] == "summary"]
    if summaries:
        print(f"Writer queue rejected records: {summaries[-1]['queueRejectedRecords']}")

    if args.replay:
        inits = {event["generation"]: event for event in outputs if event["type"] == "init"}
        partials = [event for event in outputs if event["type"] == "partial"
                    and event["generation"] in inits]
        first = next((index for index, event in enumerate(partials)
                      if event["independent"]), None)
        if first is None:
            raise ValueError("no independent partial with a matching init was captured")
        generation = partials[first]["generation"]
        chosen = [event for event in partials[first:] if event["generation"] == generation]
        with args.replay.open("wb") as replay:
            replay.write(payload(raw_output, inits[generation]))
            for event in chosen:
                replay.write(payload(raw_output, event))
        print(f"Replayed generation {generation}, {len(chosen)} partials to {args.replay}")


if __name__ == "__main__":
    main()
