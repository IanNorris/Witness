# Witness Backlog

This file tracks worthwhile product and engineering work that is not part of the
current implementation branch. Items are intentionally outcome-focused; design
details should be refined when an item is scheduled.

## Build and inference

- Reduce the ONNX Runtime/vcpkg build surface. In particular, remove unused
  operator kernels and GPU providers from CPU deployments, and investigate a
  supported prebuilt package so routine toolchain changes do not require a
  multi-hour dependency rebuild.

## Operational UI and logging

- Build a terminal dashboard showing queue lengths, camera connection health,
  stream latency, processing throughput, and the latest detection details.
  Preserve a conventional log mode for redirection and service operation.
- Rate-limit repeated FFmpeg errors without losing their first occurrence,
  total count, camera/codec context, or the timestamps of an error cluster.

## Dashboard layouts

- Allow the recent-activity view to occupy an arbitrary resizable rectangle in
  a custom layout, rather than only a full-width or full-height dock edge. This
  should make it possible to fill otherwise unusable gaps in asymmetric camera
  grids.
- Add generated layout presets driven by a per-group camera priority order,
  such as `1 large + 2 medium + 4 small`, `4 medium + 4 small`, and `16 small`.
  Keep the current hand-authored layout as the custom option.
- Explore numbered automatic slots for motion-active cameras. Fill slots in
  camera-priority order, keep the highest-priority active camera in the lowest
  numbered slot, use configurable fallback cameras when nothing is active, and
  apply hold time/hysteresis so views do not churn during adjacent events.
- Keep every camera in a dashboard group connected and primed even when it is
  temporarily absent from an automatic slot; promotion must not incur the
  camera stream's normal startup delay.

## Activity quality

- Prototype inexpensive visual-similarity grouping for adjacent clips so
  repeated grass movement, cobwebs, lighting changes, and other nearly
  identical events do not dominate recent activity. Compare perceptual hashes
  and small background-difference descriptors, using time and camera identity
  as strong grouping constraints.
- Preserve meaningful changes within a group: choose a useful representative
  frame, expose the event count/time span, and make expansion to the original
  clips straightforward. Measure false grouping on people, vehicles, animals,
  night vision transitions, and mostly static scenes before enabling it by
  default.

## Streaming diagnostics

- Add bounded, opt-in retention of encoded media around an anomaly so the exact
  access units can be replayed through software decoders offline. Redact stream
  URLs/credentials, make the storage and privacy cost explicit, and expire
  captures automatically.
- Close the corruption-detection gap for passthrough and frame-skipping cameras.
  Evaluate a low-cost bitstream validator or an on-demand validation decoder
  for viewed/problem cameras; do not restore full-time decode cost to every
  high-resolution stream without measurements.
- Add deterministic fault-injection tests for H.264 and H.265 streams (and each
  supported live container) covering truncation, packet loss, reordering,
  corrupt keyframes, timestamp loss, clustered failures, reconnects, and audio
  continuity. Verify that the web player holds the last good image, continues
  decoding/buffering, and resumes only at the correct random-access frame.
- Define recovery behaviour for generic HLS clients such as VLC and ffplay.
  They cannot consume the MSE corruption/recovery control messages, so compare
  withholding damaged dependent pictures until a keyframe, discontinuity-based
  recovery, and accepting decoder concealment without adding avoidable latency.
- Investigate the Tapo Wi-Fi stream failures at the RTSP ingest boundary. Record
  transport loss/reordering indicators and compare TCP/UDP, reorder queue,
  socket buffer, and maximum-delay settings per camera profile. Consider a Tapo
  native protocol only if measured RTSP tuning cannot provide reliable video.
