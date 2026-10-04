# PTZ control repair checkpoint

Branch: `codex/ptz-selection-controls`.

## Changes

- Destroy database query instances before PTZ discovery or nested lazy lookup.
  Query instances own the database mutex for their full lifetime.
- Publish lazily discovered clients under the context mutex; prefer an existing
  Baichuan connection before attempting HTTP discovery for movement and presets.
- Reject unknown movement commands and invalid speed/preset parameters.
- Queue gesture stop after the move acknowledgement, retaining the original
  camera ID. Cancel undispatched gestures and stop on release, pointer loss,
  blur, hidden page, camera replacement and control unmount.
- Bound browser-held movement gestures to ten seconds. Report command failures
  in the controls; keep the explicit Stop action available.
- Disable absolute zoom until supported state is returned; ignore stale polls
  after user edits, camera changes or unmount.
- Add press-and-hold relative zoom for cameras without absolute zoom support.
  Do not repeatedly poll an explicitly unsupported absolute zoom capability.
- Expose enabled saved positions through a read-only capability lookup. Position
  commands wait for a confirmed manual Stop; failed Stop cannot be bypassed.
  Absolute zoom uses the same ordered position-command lane on both client and
  server. Capability replies from an earlier camera visit cannot overwrite the
  current selection. No preset is assumed to represent the camera's home.
- Explicit fullscreen control selection is independent of automatic camera focus
  and layout dragging. Show an outline/dismiss label; Escape deselects first.
  Close PTZ when its camera is deselected, hidden, disabled or disconnected.
- Server-side movement leases arm before dispatch, expire after ten seconds and
  retry failed stops once per second, logging the first and every tenth failure.
  Linked views share their parent camera's lane. Each device has an independent
  watchdog so a slow camera does not delay another device's Stop. Preset commands
  stop outstanding manual movement before dispatch. Retained stop callbacks own
  transport clients only, with no database/context reference.

## Validation limits and remaining work

Pure movement regression tests cover acknowledgement ordering, cancellation,
camera replacement/unmount, ambiguous move failure and stop failure recovery.
These are request-adapter tests, not hardware movement tests.

The server deadline is an attempt deadline, not a firmware motor-stop guarantee.
If a transport call hangs, that device's Stop waits for it to finish to preserve
ordering. Unreachable cameras are retried; server shutdown attempts a final Stop
but a powered-off/crashed server cannot stop a camera. Baichuan acknowledges a
successful socket write, not motor execution. Distinct IDs on the same physical
device must be configured as linked views to share the lane.

Standalone server tests cover expiry without a browser Stop, ambiguous move
delivery, failed Stop retry, replacement ordering, independent camera lanes,
shutdown and preset interlocks. They use simulated transports, not camera motors.
Saved-position UI is implemented; automatic home/calibration recovery is a
separate backlog item and is not triggered here. Live camera capability/motor
validation remains outstanding. Do not claim the PTZ goal complete yet.

The query-lock fix has build validation but no isolated concurrency regression
test yet. Fullscreen selection still needs visual/keyboard testing in a browser.
Existing Sass, DLL-interface and incremental-link warnings are not fixed here.

Component-script tests exercise read-only preset discovery, filtering disabled
positions, actual preset POST selection, stale replies after switching away and
back, stale zoom polls, and zoom following Stop. These execute compiled Vue setup
code with mocked HTTP and lifecycle hooks, not browser-rendering or hardware.
