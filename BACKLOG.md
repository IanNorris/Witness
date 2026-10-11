# Witness Backlog

This file tracks work still to do. Implemented work is described in the
feature documents and Git history rather than retained as checked-off tasks.
Audio, activity, scoped automation, and DVR investigations have working first
implementations. Production validation and reliability work remain ongoing.

## Next feature priorities (10 October 2026)

1. Retroactive manual clip generation directly from the dashboard: choose a
   lookback (for example one minute), then either keep recording or finish at
   the request time. Detailed coverage and recording requirements are below.
   Include a per-camera instant-replay/DVR overlay (for example "Show this
   camera 5 minutes ago") so missed activity can be reviewed before deciding
   whether to save it. Keep the dashboard mounted behind it, provide seek and
   return-to-live controls, and allow saving the reviewed interval or continuing
   a recording. Investigate still-open DVR files: never require file rotation
   merely to replay recent footage, serve only committed/decodable media, and
   report the available live edge or gaps rather than claiming unwritten data.
   If open-segment replay is not viable for a format/client, hatch the pending
   region as "Currently saving" with a tooltip explaining that the last N
   minutes may be unavailable until finalization. Distinguish this from a true
   recording gap. Consider configurable one-minute DVR segments (currently
   five minutes, split at a keyframe) to reduce the fallback delay, and hourly
   folders for human-readable storage organization. Preserve legacy path/index
   compatibility and validate the increased file count/retention cost.
2. Clip search and grouping on the dashboard: search without leaving the live
   view, reuse existing clip filters/grouping, and keep original clips accessible.
3. Revisit installation and distribution after those features. Evaluate a
   native Windows installer/service and portable package alongside a Linux
   container/Compose deployment rather than forcing one installation method.
   Cover persistent recordings/database/config volumes, secrets, optional
   acceleration, authenticated first-run setup, upgrades, backups and rollback.
   Validate installation on a clean machine without a developer toolchain;
   default exposure should be conservative and never disable authentication.

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

## Dashboard layouts

- Make fullscreen camera-control selection explicit. Controls currently remain
  active for the last interacted-with camera until the user clicks elsewhere;
  add a clear selected-camera indication and predictable dismissal behaviour.
- Rework and repair the PTZ controls, including regressions from earlier
  attempts. Verify camera selection, supported commands, movement/stop behaviour
  and home/preset actions against the actual cameras. In the planned fullscreen/PTZ
  UI redesign, expose buttons for a configurable selection of named camera
  viewpoints and show the active logical home. Keep the UI abstraction
  camera-generic ("viewpoints"); Reolink presets are a provider implementation
  detail. Coordinate this with the dynamic-home investigation below.
- Add an optional clock to the fullscreen dashboard, using the established
  server-local-time display convention.
- Add an optional compact historical stream-health panel to the fullscreen
  dashboard. Show recent per-camera disconnects, freezes, latency and recovery
  trends at a glance rather than only instantaneous connection status.
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

- Add clip search and grouping within the dashboard, without navigating away
  or restarting live players. Scope results to the selected group/cameras and
  expose time range, object/audio tags and existing metadata filters. Reuse
  existing activity similarity grouping, with representative, count, time span
  and expansion into individual clips; do not confuse grouping with physical
  clip merging or semantic search. Keep grouping reversible and make results,
  pagination and loading/error states predictable in normal and fullscreen use.
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

- Add a retroactive manual-recording trigger with a dropdown for how far back
  to start, available from dashboard camera controls. Offer explicit actions
  such as "Record from 1 minute ago and continue" and "Save the last minute"
  (finish at the request's fixed server time, not when backfill completes).
  Use the camera's in-memory pre-roll where available, and backfill
  earlier footage from DVR/continuous recordings when the requested start is
  outside that buffer. Report unavailable gaps rather than implying complete
  coverage; preserve continuous audio/video timestamps and event metadata.
  Respect decodable keyframe boundaries and show actual covered times. Backfill
  must run off HTTP/camera workers, report progress/failure, and avoid duplicate
  media at the historical/live join. Define behavior when a recording is already
  active: do not silently terminate unrelated motion/API recordings or DVR;
  explicitly extend/reuse or create an independent requested clip as appropriate.

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

- Coalesce clips from the same camera when one stops and another starts within
  a configurable threshold (initially around one minute). Export them as one
  continuous clip and use DVR/continuous-recording segments to fill the gaps,
  while preserving the original event metadata and avoiding duplicate or
  non-monotonic audio/video timestamps.

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

- Standardise the web viewer on MSE: remove JPEG/HLS playback choices and the
  silent JPEG fallback. Unsupported codecs should retain accessible stream
  controls and offer a supported sub-stream with a clear explanation. Preserve
  still-image previews/thumbnails and external HLS integrations where needed;
  removing viewer options does not imply deleting those endpoints.

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
  recording output deliberately retain their existing timing. Aggregate repeated
  FFmpeg messages so they cannot evict reconnect/recovery evidence.

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
