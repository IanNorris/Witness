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
- Explicit fullscreen control selection is independent of automatic camera focus
  and layout dragging. Show an outline/dismiss label; Escape deselects first.
  Close PTZ when its camera is deselected, hidden, disabled or disconnected.

## Validation limits and remaining work

Pure movement regression tests cover acknowledgement ordering, cancellation,
camera replacement/unmount, ambiguous move failure and stop failure recovery.
These are request-adapter tests, not hardware movement tests.

The browser deadline bounds the gesture only: it cannot guarantee a firmware Stop
when HTTP or camera transport hangs. A server-side device-aware movement lease
and ordering across tabs/linked camera IDs remain required. Independent control
instances do not share a command queue. Preset/home UI and live camera capability
validation are also outstanding. Do not claim the PTZ goal complete yet.

The query-lock fix has build validation but no isolated concurrency regression
test yet. Fullscreen selection still needs visual/keyboard testing in a browser.
Existing Sass, DLL-interface and incremental-link warnings are not fixed here.
