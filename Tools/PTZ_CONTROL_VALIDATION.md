# PTZ control repair checkpoint

Branch: `codex/ptz-selection-controls`.

## Changes

- Destroy database query instances before PTZ discovery or nested lazy lookup.
  Query instances own their prepared-query mutex for their full lifetime.
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
- Fullscreen camera clicks select controls without toggling recording. An
  explicit labelled Record/Stop recording action replaces that hidden shortcut;
  normal tile-view recording shortcuts remain unchanged. Accept both pointer
  and click-based selection (including accessibility-generated clicks).
- Fullscreen PTZ uses a named floating panel outside the clipped media tile.
  Pointer interaction with that panel retains its owner selection. Camera
  changes and Escape still close it; small tile sizes do not clip the controls.
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

The query-lock fix now has an isolated native concurrency regression using the
production snapshot helpers, SQL extracted from `Database.cpp`, and the actual
SQLite query wrapper. A deliberately stalled simulated discovery holds an owned
snapshot while another camera reuses the same queries and updates SQLite text.
It verifies that queries remain available and earlier strings remain unchanged;
NULL, missing and linked-camera cases are covered too. These are per-query mutex
checks, not a claim that every DB operation or HTTP route is nonblocking.
The registered `PtzMovementLeaseTests` and `PtzConfigurationTests` targets passed
on Windows with a 15-second external process deadline. They do not connect to
any camera or move a motor. Linux compilation remains unverified.

```powershell
cmake --build build-vs2026 --config RelWithDebInfo --target PtzMovementLeaseTests PtzConfigurationTests
build-vs2026/bin/RelWithDebInfo/PtzMovementLeaseTests.exe
build-vs2026/bin/RelWithDebInfo/PtzConfigurationTests.exe WitnessServer/Source/Database.cpp
```

Computer-use browser checks against the built UI on a loopback-only
synthetic fixture verified selection, switching camera/closing the prior panel,
saved-position selection, and two-stage Escape dismissal. No physical camera,
real authentication session or production database was involved. The visual
check exposed the fullscreen recording side effect and in-tile panel clipping,
which were repaired. The test fixture cannot forward camera commands.
Later build-health branches resolved the DLL-interface and incremental-link
warnings; third-party Sass deprecations remain separate work.

Component-script tests exercise read-only preset discovery, filtering disabled
positions, actual preset POST selection, stale replies after switching away and
back, stale zoom polls, and zoom following Stop. These execute compiled Vue setup
code with mocked HTTP and lifecycle hooks, not browser-rendering or hardware.
Additional fullscreen component tests cover selection independent of focus,
Escape ordering, outside clicks, floating-panel ownership, layout-editor mode,
no fullscreen click-to-record, preserving tile-view recording, and PTZ dismissal.

## Read-only physical capability checkpoint (4 October)

`PtzCapabilityProbe` calls the production Reolink HTTP transport's authentication,
`GetPosition`, `GetZoomFocus` and `GetPresets` methods only. No motor movement,
zoom/preset writes, reboot or calibration commands are reachable. Its wrapper
reads the supplied SQLite database in read-only mode, follows the configured
physical parent, and passes credentials through environment variables rather
than process arguments. Only allowlisted numeric result lines are relayed;
transport logs, hosts, credentials and preset names are suppressed.

```powershell
cmake --build build-vs2026 --config RelWithDebInfo --target PtzCapabilityProbe
python Tools/Tests/run_ptz_capability_probe.py --database C:/Users/icsta/Downloads/server.db --probe build-vs2026/bin/RelWithDebInfo/PtzCapabilityProbe.exe --camera 10
```

Actual camera 10 authenticated and reported valid zoom state (range 1000–6000,
position 2155) and one enabled saved position in 1,029 ms. Camera 12 authenticated
and reported the same zoom range (position 1000) and eight enabled saved positions
in 10,402 ms. Both returned an invalid absolute pan/tilt position through the
current parser/API. This does not establish that their firmware has no alternative
position API. Results: `artifacts/ptz-camera10-readonly-capabilities.log` and
`artifacts/ptz-camera12-readonly-capabilities.log`.

The wrapper regression verifies linked-parent lookup, URL credential fallback,
no database-byte changes, bounded subprocess timeout, no credentials in argv
and rejection of arbitrary/numeric-looking log output. The final probe build
has no compiler warnings; `ReolinkClient.h` now directly includes its memory
and vector dependencies instead of relying on earlier includes. Linux execution
and physical movement/Stop behavior are still not validated by this read-only
check. Explicit permission is required before the latter test is attempted.
