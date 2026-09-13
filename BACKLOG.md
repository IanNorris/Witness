# Witness Backlog

This file tracks worthwhile product and engineering work that is not part of the
current implementation branch. Items are intentionally outcome-focused; design
details should be refined when an item is scheduled.

## Live viewing

- Add a per-camera audio toggle to the web UI and remember the preference in
  browser storage.
- Investigate an optional "audio while motion is active" mode. Define sensible
  lead-in/hold times so audio does not chatter as motion state changes, and make
  the privacy implications clear in the UI.

## Startup and maintenance

- Move cleanup of backlog left by earlier server sessions off the blocking
  startup path. Start serving and connecting cameras first, then drain cleanup
  work in a bounded background lane with progress and queue-length diagnostics.

## Build and inference

- Reduce the ONNX Runtime/vcpkg build surface. In particular, remove unused
  operator kernels and GPU providers from CPU deployments, and investigate a
  supported prebuilt package so routine toolchain changes do not require a
  multi-hour dependency rebuild.

## Operational UI and logging

- Reduce routine TTY log volume and make detail selectable by category and
  severity.
- Build a terminal dashboard showing queue lengths, camera connection health,
  stream latency, processing throughput, and the latest detection details.
  Preserve a conventional log mode for redirection and service operation.

## Dashboard

- Add a persistent strip of interesting/recent clips along the bottom of the
  dashboard. Use the representative detection frame, remember the selected
  clips in the web UI, and open the associated clip at the relevant time when
  clicked. Reuse the clip-dashboard card and playback behaviour where possible.

## Streaming diagnostics

- Add a codec- and container-neutral rolling "black box" for packet provenance,
  timestamp transformations, fragment integrity, delivery, and browser
  presentation. Capture the preceding window when an anomaly occurs and allow
  optional retention of encoded media for offline software-decoder validation.
