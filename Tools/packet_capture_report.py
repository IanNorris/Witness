#!/usr/bin/env python3
"""Inspect a bounded Witness packet capture, optionally reconstruct its fMP4."""

import argparse
import collections
import json
import math
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


def percentiles_ms(values: list[int]) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {}
    return {name: ordered[max(0, math.ceil(len(ordered) * fraction) - 1)] / 1000
            for name, fraction in (("p50Ms", .5), ("p95Ms", .95), ("p99Ms", .99), ("maxMs", 1))}


def delivery_timing(events: list[dict]) -> dict:
    """Use independent monotonic observations, never faulty source DTS, as time."""
    videos = [event for event in events if event["type"] == "input" and not event["audio"]]
    timed = [event for event in videos if isinstance(event.get("observedSteadyUs"), int)]
    result = {"videoPackets": len(videos), "timedVideoPackets": len(timed)}
    # Do not estimate delivery rate from a partially timestamped old capture.
    if len(timed) != len(videos) or len(timed) < 2:
        result["available"] = False
        return result
    gaps = [right["observedSteadyUs"] - left["observedSteadyUs"]
            for left, right in zip(timed, timed[1:])]
    result["backwardObservationCount"] = sum(gap < 0 for gap in gaps)
    if result["backwardObservationCount"]:
        result["available"] = False
        return result
    elapsed = timed[-1]["observedSteadyUs"] - timed[0]["observedSteadyUs"]
    result["available"] = elapsed > 0
    if elapsed <= 0:
        return result
    result["observedSpanMs"] = elapsed / 1000
    result["deliveredFps"] = (len(timed) - 1) * 1_000_000 / elapsed
    result["arrivalGaps"] = percentiles_ms(gaps)
    result["longGapsOver250Ms"] = sum(gap > 250000 for gap in gaps)
    inputs = {(event["activityId"], event["audio"]): event for event in timed}
    to_mux, mux_work = [], []
    for decision in (event for event in events if event["type"] == "decision"):
        source = inputs.get((decision["activityId"], decision["audio"]))
        mux = decision.get("muxObservedSteadyUs")
        done = decision.get("decisionSteadyUs")
        if source is not None and isinstance(mux, int) and mux >= source["observedSteadyUs"]:
            to_mux.append(mux - source["observedSteadyUs"])
        if isinstance(mux, int) and isinstance(done, int) and done >= mux:
            mux_work.append(done - mux)
    result["demuxToMuxSamples"] = len(to_mux)
    result["demuxToMuxIncludingLookahead"] = percentiles_ms(to_mux)
    result["muxDecisionSamples"] = len(mux_work)
    result["muxToDecision"] = percentiles_ms(mux_work)
    return result


def replay_input_video(events: list[dict], blob: bytes, destination: Path) -> tuple[int, int]:
    """Replay exact Annex-B access units without Witness's output timestamp policy."""
    streams = [row for row in events if row["type"] == "stream" and
               not row["audio"] and row["codecId"] in (27, 173)]
    if len(streams) != 1:
        raise ValueError("input replay requires exactly one H.264/HEVC video stream")
    stream = streams[0]["streamIndex"]
    extra = next((row for row in events if row["type"] == "extradata" and
                  row["streamIndex"] == stream), None)
    packets = [row for row in events if row["type"] == "input" and
               not row["audio"] and row["streamIndex"] == stream]
    first = next((index for index, row in enumerate(packets) if row["flags"] & 1), None)
    if extra is None or first is None:
        raise ValueError("input replay needs captured parameter sets and a keyframe")
    chunks = [payload(blob, extra)] + [payload(blob, row) for row in packets[first:]]
    if any(not chunk.startswith((b"\x00\x00\x01", b"\x00\x00\x00\x01")) for chunk in chunks):
        raise ValueError("input replay supports Annex-B only; refusing length-prefixed data")
    # Validate before creating anything; never replace another recording.
    with destination.open("xb") as replay:
        for chunk in chunks:
            replay.write(chunk)
    return packets[first]["activityId"], len(packets) - first


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("--replay", type=Path, help="write a playable output fMP4")
    parser.add_argument("--input-video", type=Path,
                        help="write exact Annex-B H.264/HEVC input from the first captured keyframe")
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
    print("Delivery timing: " + json.dumps(delivery_timing(events), sort_keys=True))
    summaries = [event for event in events if event["type"] == "summary"]
    if summaries:
        print(f"Writer queue rejected records: {summaries[-1]['queueRejectedRecords']}")

    if args.input_video:
        if bad_hashes or (summaries and summaries[-1]["queueRejectedRecords"]):
            raise ValueError("refusing input replay from a corrupt or queue-truncated capture")
        activity, count = replay_input_video(events, raw_input, args.input_video)
        print(f"Input replay: {count} access units starting at activity {activity} to {args.input_video}")

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
