# Witness Backlog

This file tracks work still to do. Implemented work is described in the
feature documents and Git history rather than retained as checked-off tasks.
Current priorities are production validation of the retention-stall fix,
reliable DVR playback, and measured build/header/ABI cleanup. Audio, activity,
scoped automation, and DVR investigations have working first implementations.

## Build and inference

- Roll out the checked-in stable VS2026/vcpkg environment to CI and production,
  publish a reusable release-only dependency artifact, and verify a clean
  bootstrap on another machine. The triplet, fingerprint guard, explicit
  bootstrap command, local binary cache, and old-build coexistence are in
  [BUILDING.md](BUILDING.md). Add a CI check that routine configure cannot
  install packages; retain the legacy tree until the new one is proven.
- Re-evaluate the CMake and vcpkg foundations rather than assuming they remain
  the long-term build/package solution. Compare at least: a native/generated
  Visual Studio build with an explicit Linux build path; Meson; Conan-backed
  builds; and a tightly pinned, non-mutating CMake/vcpkg configuration. The
  decision must cover reproducible Windows and Linux builds, IDE integration,
  prebuilt dependency consumption, offline/cache behaviour, security updates,
  CI packaging, incremental build time, and the cost of maintaining one versus
  two platform build descriptions. A replacement must coexist with the current
  build until it produces equivalent binaries and tests; ordinary developer
  builds must never update package metadata or invalidate dependencies merely
  because a global tool installation changed.

- Reduce the ONNX Runtime/vcpkg build surface. In particular, remove unused
  operator kernels and GPU providers from CPU deployments, and investigate a
  supported prebuilt package so routine toolchain changes do not require a
  multi-hour dependency rebuild.
- Extend the existing build profiler beyond the initial Crow PCH and header
  isolation pass (measured in [BUILDING.md](BUILDING.md)). Compare clean and
  incremental rebuild fan-out, then pursue narrower PIMPL boundaries where
  they reduce both compile time and DLL ABI warnings.
- Clean up the native DLL ABI boundaries currently producing MSVC C4251
  warnings. Inventory exported classes that expose STL containers, strings,
  callbacks, mutexes, smart pointers, or chrono types; move implementation
  state behind PIMPL where it provides a stable ownership and ABI boundary,
  and keep deliberately header-defined value types explicit rather than merely
  suppressing the warning globally. Add a small cross-DLL construction and
  destruction test so allocator/runtime mismatches are caught.

## Operational UI and logging

The initial Windows console dashboard and the standalone Windows/Linux boundary
are described in [TERMINAL_DASHBOARD.md](TERMINAL_DASHBOARD.md).

- Build the standalone Windows named-pipe TUI client described in
  [TERMINAL_DASHBOARD.md](TERMINAL_DASHBOARD.md); the in-process console UI and
  plain-log mode already exist. Add stream freshness, throughput, reconnect-age
  and recent detection details to the shared operational snapshot.
- Extend the existing health dashboard with host CPU *availability*,
  colour-coded queue/latency thresholds, per-camera throughput and
  per-activation versus all-frame processing costs. Include scaling/JPEG work
  and activation counts in its copy/export table.
- Add reliable hardware decoder/encoder capacity and fallback information to
  the combined server/browser health view; avoid treating vendor-specific
  session limits as universal.
- Persist a bounded history of browser/client health reports server-side,
  keyed by logged-in username plus a stable locally stored browser/session
  identifier. A health export from any browser should include recent telemetry
  from all known clients, including hidden tabs and sessions that have since
  disconnected, so opening the Health page cannot erase or perturb the evidence
  of a playback failure.
- Show dismissible warning toasts when a camera becomes unhealthy. Define
  health using connection stability, stream freshness, decode errors, latency,
  and sustained queue pressure, with rate limiting and recovery notification so
  intermittent cameras do not continuously interrupt the operator.

## Audio intelligence

- Evaluate and tune the opt-in, post-clip sound classifier against labelled
  real-camera recordings, especially vehicle starts, footsteps, barking,
  speech and wind across different microphones. Add confidence/false-positive
  controls and, only after measurement, consider short-window live audio
  triggers without retaining continuous audio. See
  [AUDIO_INTELLIGENCE.md](AUDIO_INTELLIGENCE.md).

## Dashboard layouts

- Present three explicit dashboard modes: **Tiles**, **Layout**, and **Full
  Screen**. Tiles should remain the conventional responsive grid; Layout should
  use the configurable per-group canvas and automatic slots; Full Screen should
  be the distraction-free presentation of the selected layout.
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

The grouping, user-relevance, and object-persistence design is recorded in
[ACTIVITY_INTELLIGENCE.md](ACTIVITY_INTELLIGENCE.md). It identifies preserving
empty/baseline observation evidence as a prerequisite for reliable persistence.

- In recent activity, use the interesting-object bounding boxes to select a
  tighter thumbnail crop/zoom. When face extraction produced a useful face
  image, offer it as an overlay without obscuring the wider event context.
- Remove clips from recent activity when post-processing finds no object or
  event worth retaining. Keep the underlying retention/audit policy separate so
  hiding low-value activity does not silently delete evidence unless configured.
- Measure and tune the existing visual-similarity grouping on people,
  vehicles, animals, night-vision transitions and static scenes. Expose why a
  group formed and make false groups easy to split or mark uninteresting;
  retain the current representative/time-span/expansion behavior.

## Clip generation

- Coalesce clips from the same camera when one stops and another starts within
  a configurable threshold (initially around one minute). Export them as one
  continuous clip and use DVR/continuous-recording segments to fill the gaps,
  while preserving the original event metadata and avoiding duplicate or
  non-monotonic audio/video timestamps.

## DVR and historical search

- Validate the new opt-in DVR page with real recordings, especially missing
  segments, concurrent cameras, browser media errors, and the manual bisection
  flow. Synchronize selected cameras to one viewed wall-clock position during
  continuous playback; currently they are aligned on explicit seek but may
  drift independently. Move thumbnail decoding and remaining slow DVR/database
  operations off Crow request threads after measuring handler latency.

## Streaming diagnostics

- Investigate malformed Reolink AAC timestamps in the live MP4 muxer. The
  17 September health export showed all Reolink main and preview streams
  continuously emitting negative audio packet-duration and missing-PTS
  warnings, sometimes including `audio-packet / nonMonotonicOutput`. Capture
  raw AAC DTS/PTS/duration, attribute stream-1 mux warnings as audio, then
  validate whether AAC PTS can safely follow DTS and duration can be derived
  from samples/timebase. Aggregate repeated messages so they cannot evict
  reconnect and recovery evidence from the diagnostic ring.

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
