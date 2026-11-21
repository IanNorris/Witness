# Witness Backlog

This file tracks work still to do. Implemented work is described in the
feature documents and Git history rather than retained as checked-off tasks.
Audio, activity, scoped automation, and DVR investigations have working first
implementations. Production validation and reliability work remain ongoing.

## Near-term work (reviewed 11 October 2026)

Prioritise smaller usability/reliability improvements before the deferred
spatial-reconstruction and provenance projects. Dashboard replay, retroactive
capture, clip search/grouping and layout automation are implemented; remaining
validation and follow-ups are listed in their sections below.

- Simplify viewer choices to MSE and finish codec/failure feedback.
- Add the compact historical health panel. The optional fullscreen server-local
  clock is implemented; validate placement with production layouts.
- Improve recent-activity thumbnails and low-value-event filtering.
- Revisit installation and distribution. Evaluate a native Windows
  installer/service and portable package alongside a Linux container/Compose
  deployment rather than forcing one installation method. Cover persistent
  recordings/database/config volumes, secrets, optional acceleration,
  authenticated first-run setup, upgrades, backups and rollback. Validate on a
  clean machine without a developer toolchain; exposure should be conservative
  and never disable authentication.

Provenance is deferred behind these usability and deployment priorities. AAC
live/recorded audio coverage is also recorded below for a compatibility pass.

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

- Further reduce unused ONNX operator kernels if measurements justify it.
  CPU-only/prebuilt ONNX deployment and dependency reuse already exist; validate
  clean-machine consumption and security-update procedures rather than rebuild
  that implementation. Avoid multi-hour routine dependency rebuilds.
- Extend the existing build profiler beyond the initial Crow PCH and header
  isolation pass (measured in [BUILDING.md](BUILDING.md)). Compare clean and
  incremental rebuild fan-out, then pursue narrower PIMPL boundaries where
  they reduce compile time while preserving the now-tested DLL ABI boundaries.
- Keep the now-warning-clean native DLL ownership boundaries covered by the
  cross-DLL lifetime tests. Pursue further PIMPL changes only for measured
  rebuild/ownership benefits, not an already-resolved C4251 cleanup task.
  Resolve remaining third-party Sass deprecations and the web chunk-size
  advisory without globally suppressing warnings. See [BUILDING.md](BUILDING.md)
  for the completed ownership and linker-warning passes.

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
- Extend the existing browser playback-resource snapshots and suspected
  decoder-pressure warning with capability/fallback evidence where available;
  do not claim an exact hardware capacity from loaded-video counts. Preserve
  unsupported-codec versus decode-failure distinctions in both browsers.
- Extend the implemented bounded, server-persisted client-health history to
  retain the new decoder-resource context and richer DVR failure fields.
  Browser-local exports already retain them, but the server payload whitelist
  still strips some details. Validate exports from another browser and after
  restart without losing the causal evidence from disconnected sessions.
- Show dismissible warning toasts when a camera becomes unhealthy. Define
  health using connection stability, stream freshness, decode errors, latency,
  and sustained queue pressure, with rate limiting and recovery notification so
  intermittent cameras do not continuously interrupt the operator.

## Audio intelligence

- Audit and complete AAC audio support for live playback and recordings where
  feasible. Live AAC passthrough already exists; identify unsupported profiles,
  camera configurations and recording paths rather than treating this as a new
  codec implementation. Verify A/V synchronization, seeking, reconnection,
  Main/Preview switching and export with H.264 and H.265 video. Prefer lossless
  passthrough where compatible; make any required transcoding and CPU cost
  explicit, and report unsupported audio instead of silently losing it.
- Evaluate and tune the opt-in, post-clip sound classifier against labelled
  real-camera recordings, especially vehicle starts, footsteps, barking,
  speech and wind across different microphones. Add confidence/false-positive
  controls and, only after measurement, consider short-window live audio
  triggers without retaining continuous audio. See
  [AUDIO_INTELLIGENCE.md](AUDIO_INTELLIGENCE.md).
- Add audio-event lanes to the all-clips timeline (DVR audio events and clip
  labels already exist). Explore opt-in wind-only muting during playback, with
  confidence/hysteresis and audible-event overrides; do not suppress speech,
  sirens or other interesting sounds merely because wind is also present.

## Dashboard layouts

- Extend the repaired, isolated PTZ controls with buttons for a configurable
  selection of named camera viewpoints and show the active logical home. Keep
  the UI abstraction
  camera-generic ("viewpoints"); Reolink presets are a provider implementation
  detail. Coordinate this with the dynamic-home investigation below.
- Add an optional clock to the fullscreen dashboard, using the established
  server-local-time display convention.
- Add an optional compact historical stream-health panel to the fullscreen
  dashboard. Show recent per-camera disconnects, freezes, latency and recovery
  trends at a glance rather than only instantaneous connection status.
- Validate the implemented layout presets, activity rectangles and automatic
  slots on larger groups and small screens. Hidden group cameras remain mounted
  to avoid cold promotion; measure decoder/resource cost and balance priming
  against supported sub-stream selection rather than starting duplicate feeds.

## Activity quality

The grouping, user-relevance, and object-persistence design is recorded in
[ACTIVITY_INTELLIGENCE.md](ACTIVITY_INTELLIGENCE.md). It identifies preserving
empty/baseline observation evidence as a prerequisite for reliable persistence.

- Validate the implemented dashboard clip search/grouping with production data,
  particularly group/camera scoping, tag/time filters, pagination, loading/error
  states and fullscreen interaction. Keep individual clips accessible and avoid
  interrupting mounted live players.
- Validate the new optional recent-activity object-focus thumbnails with
  production data. Saved non-baseline detection crops are used when available,
  with full-scene fallback; visual grouping still compares original thumbnails.
  Add an extracted-face overlay without obscuring the wider event context.
- Remove clips from recent activity when post-processing finds no object or
  event worth retaining. Keep the underlying retention/audit policy separate so
  hiding low-value activity does not silently delete evidence unless configured.
- Measure and tune the existing visual-similarity grouping on people,
  vehicles, animals, night-vision transitions and static scenes. Expose why a
  group formed and make false groups easy to split or mark uninteresting;
  retain the current representative/time-span/expansion behavior.
- Implement the remaining user-relevance and usual-place/object-persistence
  stages in [ACTIVITY_INTELLIGENCE.md](ACTIVITY_INTELLIGENCE.md): uninteresting
  examples, retained baseline observations, appearance/movement/disappearance
  history and day/night resets. Existing thumbnail grouping alone does not
  establish persistent object identity or prove that an object remained present.

## Spatial scene understanding (deferred research)

Prioritise smaller usability/reliability improvements before this large feature.

- Explore single-camera depth estimation as an optional aid to spatial zones,
  approaching/leaving trajectories and persistent objects in usual locations.
  Do not require overlapping cameras: start with a ground map, user-marked
  landmarks and known distances rather than a full textured 3D property model.
  Distinguish inferred relative depth, estimated metric depth and triangulated
  measurements; retain uncertainty and never present estimates as exact evidence.
- Evaluate Depth Anything V2 (including metric variants), Apple Depth Pro and
  Depth Anything 3. Benchmark CPU cost, memory, model licensing, outdoor CCTV,
  infrared/night imagery and temporal stability before selecting a model.
  Prefer cached static-scene estimates with occasional refresh where viable;
  validate how moving objects, lighting and scene changes affect those maps.
- Add optional multi-camera track fusion/3D triangulation where overlapping
  views provide a useful baseline (including vertically separated cameras).
  Validate calibration, distortion, metric scale, capture-time alignment,
  correspondence and occlusion; distant targets with a small baseline may
  remain poorly constrained. Keep single-camera operation useful independently.
- For PTZ, initially calibrate verified presets and zoom settings separately.
  Invalidate spatial mapping during movement or unverified viewpoint changes;
  investigate pose estimation later rather than assuming motor coordinates are
  accurate or synchronised with frames. A PTZ sweep alone is not stereo depth.

Research starting points:

- [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2)
- [Apple Depth Pro](https://github.com/apple-aiml-research/ml-depth-pro)
- [Depth Anything 3](https://github.com/ByteDance-Seed/Depth-Anything-3)
- [NVIDIA multi-view 3D tracking](https://docs.nvidia.com/metropolis/deepstream/9.1/text/DS_MV3DT.html)
- [COLMAP reconstruction](https://colmap.github.io/tutorial.html)
- [DeepPTZ calibration](https://openaccess.thecvf.com/content_WACV_2020/html/Zhang_DeepPTZ_Deep_Self-Calibration_for_PTZ_Cameras_WACV_2020_paper.html)

## Clip generation

- Production-validate the implemented retroactive save/continue jobs, especially
  retained-to-DVR/live joins, reconnects, active segments, missing coverage,
  cancellation and restart recovery. Preserve independent capture ownership:
  do not stop unrelated motion/API recordings. Check reported coverage against
  actual decoded playback, including audio and keyframe boundaries.
- Coalesce clips from the same camera when one stops and another starts within
  a configurable threshold (initially around one minute). Export them as one
  continuous clip and use DVR/continuous-recording segments to fill the gaps,
  while preserving the original event metadata and avoiding duplicate or
  non-monotonic audio/video timestamps.
- Record both streams of a camera (for example HEVC main and H.264 sub) as
  alternate renditions of the same clip, because the HEVC main stream
  occasionally glitches in recordings. Paired cameras (PTZ wide/focus) extend
  this to one clip with up to four renditions: wide main/sub and focus
  main/sub.
  - **Model:** keep one clip and one event timeline, with the renditions
    linked to it, rather than separate clips.
  - **Joining:** align renditions on presentation clocks, as DVR captures
    already do, so you can switch between them or splice around a damaged
    span. Record coverage and gaps per rendition.
  - **Clip viewer:** a rendition dropdown (camera × stream) that keeps the
    playback position when switching, and marks renditions that are missing
    or damaged over the current span.
  - **Storage:** retention and storage accounting count each rendition.
    Recording the sub-stream should be opt-in per camera, with its cost shown
    in the storage settings.
  - **Detection:** detection, trails and tags stay attached to the single
    clip (computed on one designated rendition), not duplicated per stream.

## Evidence provenance (deferred)

- Explore human-readable, verifiable recording provenance. Reviewers' ability
  to understand the evidence matters as much as the mechanism: camera-generated
  visible timestamps are the preferred starting point for understandable time
  context. An overlay alone is not tamper evidence or proof that the clock was
  correct, and no technical feature should promise legal admissibility.
- Preserve original encoded recordings and camera overlays. Avoid mandatory
  server burn-in/re-encoding; optional Witness timestamped, redacted or joined
  exports should be labelled as derived copies linked to their originals.
- Record camera-reported wall time where available, server receive time,
  original PTS/DTS, timing repairs, clock offsets/synchronization observations
  and clock changes separately. Media timestamps are not inherently calendar
  time; receive time includes transport delay. Investigate local time-server
  support for internet-blocked cameras and explicitly flag uncertain clocks.
- Consider hashing/signing finalized segments and their provenance manifests,
  linking sequences and documenting gaps/restarts/retention. Protect signing
  keys and retain independent checkpoints; optional RFC 3161 timestamping of
  hashes proves existence by an external time, not the exact capture time.
  A recorder signature establishes trust from signing/receipt, not camera capture.
- Investigate preserving/verifying camera signatures where actually supported.
  ONVIF Media Signing covers H.264/H.265 and currently excludes audio; generic
  ONVIF support does not imply signing. Package originals, manifests and an
  independently usable verifier/report for exports. Treat Frigate's signing
  support as unverified, not an established absence or reason to claim superiority.

Research references:

- [Video evidence recovery and clock checks](https://www.gov.uk/government/publications/recovery-and-acquisition-of-video-evidence/recovery-and-aquisition-of-video-evidence-v30)
- [Axis camera-signed video](https://developer.axis.com/video-streaming-and-recording/signed-video/)
- [ONVIF Media Signing](https://www.onvif.org/add-on/media-signing-add-on/)
- [Synology evidence integrity](https://www.synology.com/en-global/surveillance/feature/secure)
- [RFC 3161 trusted timestamping](https://www.rfc-editor.org/info/rfc3161/)

## DVR and historical search

- Revisit cache-folder organisation: large flat directories are impractical to
  browse manually (Explorer can take minutes to open them). Evaluate per-camera,
  date and hour folders with readable filenames for DVR, previews and detection
  assets; preserve existing database paths and support legacy files. Plan a
  bounded background migration, retention/cleanup compatibility and tests so
  reorganising storage cannot lose recordings or block startup.

- Validate the new opt-in DVR page with real recordings, especially missing
  segments, concurrent cameras, browser media errors, and the manual bisection
  flow. Synchronize selected cameras to one viewed wall-clock position during
  continuous playback; currently they are aligned on explicit seek but may
  drift independently. Move thumbnail decoding and remaining slow DVR/database
  operations off Crow request threads after measuring handler latency.

## Streaming diagnostics

- Validate MSE-only live viewing across supported browsers, including old
  JPEG/HLS preference migration and unsupported Main → Auto/Sub recovery.
  Still-image previews/thumbnails and external HLS endpoints remain supported.

- Investigate two reported Reolink camera-firmware behaviours; research known
  reports and supported workarounds, recording affected models/firmware before
  attributing either symptom to a general provider issue:
  - Failure to return correctly to the home position, sometimes leaving the
    camera stuck. Record affected models/firmware and investigate the following
    behaviour and recovery options rather than assuming API support or semantics:
    - Model multiple named camera viewpoints, backed by stored Reolink presets
      for that provider. Let Witness select a dynamic logical home from those
      viewpoints using state such as day/night or vehicle present/absent.
      Make it configurable whether a state change moves the camera immediately
      or only changes the destination used after tracking finishes.
    - Investigate `GetPtzPreset`, `SetPtzPreset`, `PtzCtrl`/`ToPos`,
      `GetPtzGuard` and `SetPtzGuard` on the actual models/firmware. In particular,
      determine whether `SetPtzGuard` can make an existing preset the native
      guard/monitor point without first moving there and resetting the guard
      from the current position. Prefer native dynamic guard switching if
      practical and reliable; otherwise have Witness explicitly return to the
      active viewpoint preset after tracking finishes, with a configurable
      settling delay. Verify how native auto-return and tracking interact with
      either approach, and avoid competing return commands.
    - After `ToPos` and movement settling, compare the resulting view with a
      saved reference image for the selected viewpoint to detect preset or
      calibration drift. Compare readable PTZ coordinates as another detection
      option if available and trustworthy. Account for day/night appearance,
      vehicles and other legitimate scene changes so these do not become
      position errors.
    - Stage recovery only after verification indicates a position problem:
      first evaluate a bounded retry of the active viewpoint, then optional
      calibration followed by return to that viewpoint and repeat verification.
      Investigate `GetPtzCheckState` and `PtzCheck`, including whether calibration
      affects stored presets or the guard point. Treat command availability,
      status semantics and firmware behaviour as investigation findings.
      Keep automatic recovery opt-in and rate-limited, avoid fighting manual
      PTZ operation or active tracking, and assess periodic recovery only after
      verification and evidence justify it.
  - Failure to switch correctly to night vision after the light has been on,
    producing a black or mostly black image. Evaluate sustained-dark-image
    detection and supported night-vision/light toggles as possible recovery.
    Distinguish this from genuine darkness, obstruction or a failed stream,
    and make any automatic camera-changing recovery opt-in and rate-limited.

- Production-validate the guarded Reolink live AAC sample clock (30 September
  investigation, `Tools/REOLINK_HITCH_2026-09-30.txt`). Local captures confirmed
  timestamp jitter and one-tick muxed audio samples; fixed live tests eliminated
  negative-duration/missing-PTS warnings. Retain investigation of rare clock
  resets, long-session A/V drift and the original production hitch; short replay
  tests alone cannot prove those solved. Other audio codecs/ASC extensions and
  recording output deliberately retain their existing timing. Validate the
  implemented FFmpeg warning clustering during long sessions so repeated
  messages cannot evict reconnect/recovery evidence.

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
