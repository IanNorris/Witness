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

## Synthetic production HTTP transport checkpoint (4 October)

`PtzTransportProbe` uses the real Reolink client, but its destination is hardcoded
to `127.0.0.1` with synthetic credentials. It has no host argument, database lookup
or auto-detection path. The Python test starts a disposable loopback HTTP camera;
no physical camera command is sent. The target is `EXCLUDE_FROM_ALL` and is not
included in deployment folders.

```powershell
cmake --build build-vs2026 --config RelWithDebInfo --target PtzTransportProbe
$env:WITNESS_PTZ_TRANSPORT_PROBE=(Resolve-Path build-vs2026/bin/RelWithDebInfo/PtzTransportProbe.exe).Path
python Tools/Tests/test_ptz_transport.py
```

The pre-fix run recorded thirteen failed assertions: malformed HTTP 200 responses
such as `[{}]` were acknowledged as successful, fractional codes could truncate
to zero, and other malformed shapes raised exceptions. This included Stop and
the alternative zoom operation. A successful-looking ambiguous Stop can clear
the movement lease's retry state even though the device did not acknowledge it.

Movement/Stop, presets and alternative zoom acknowledgements now require a
single-object response containing a finite, integral, in-range numeric `code`,
and only zero is success. Missing, boolean/string, fractional or oversized codes,
null/string objects, multiple responses, invalid JSON and HTTP/device errors
cannot acknowledge a command. Invalid preset acknowledgements also set an error.
The zoom fallback keeps its retry JSON separate: previously it replaced the
original JSON storage while retaining a reference into that storage for error
reporting. Original error details are type-checked before reading them.

All five test methods / 38 synthetic command cases pass, including low-speed
movement/Stop/preset wire fields, malformed acknowledgements, zoom fallback and
one-time HTTP 401 re-authentication followed by an identical Stop. The independent
movement-lease expiry, ambiguous-delivery, failed-Stop retry, linked ordering,
shutdown and preset tests also pass. These prove protocol/lease behavior against
a synthetic device, not motor movement on real firmware or physical Stop latency.
The latter still requires explicit permission.

Final runtime build: `artifacts/ptz-ack-runtime-build.log`, no C++/linker warnings
or dependency installation; existing third-party web warnings remain visible.
All 44 UI regressions pass. Logs: `artifacts/ptz-transport-before.log`,
`artifacts/ptz-transport-final.log`, `artifacts/ptz-lease-after-ack.log`, and
`artifacts/ptz-ack-ui-tests.log`.

Deployment: `build-vs2026/deploy/ptz-ack-49630fd8b90c`, native ID `49630fd8b90c`,
web ID `439c7eef7414`. The read-only verifier confirms all 96 files /
179,875,602 bytes match runtime output and both native modules contain the native
ID. Report: `artifacts/ptz-ack-package-verification-20261004.json`. This package
includes the prior ordered improvements; it was not deployed, merged to main
or pushed. The original package and user's backlog edits remain unchanged.

### Absolute controls and preset edits

The follow-up synthetic check extends the same production transport to absolute
zoom/focus, saving presets and deleting presets. Before this pass, absolute
controls and preset saves still accepted missing/fractional success codes, while
delete accepted any nonempty response (including a device error or invalid JSON).
Preset names were concatenated directly into JSON, so quotes, backslashes and
newlines could make otherwise valid requests malformed. The pre-fix run records
25 failing assertions in `artifacts/ptz-write-before.log`.

All four methods now use the explicit acknowledgement validator, preserving
transport failures and setting operation-specific invalid/device error text.
Preset names are serialized with Crow's JSON string encoder. Read-only camera
queries, reboot behavior, physical preset values and authentication are unchanged.
The probe remains hardcoded to loopback with synthetic credentials; it cannot
select a real camera or import its credentials.

All six methods / 90 synthetic command cases pass in
`artifacts/ptz-write-after.log`, including absolute control wire fields, round-trip
preset names with quote/backslash/newline characters, malformed acknowledgements,
HTTP/device failures, zoom fallback and Stop re-authentication. Server shutdown
polling in the Python fixture is reduced to 10 ms to avoid a needless half-second
delay per case; command/network timeouts remain bounded. Lease tests and all 44
UI tests pass again (`artifacts/ptz-write-lease.log`, `artifacts/ptz-write-ui-tests.log`).

Final combined package: `build-vs2026/deploy/ptz-writes-7db5a3d55a55`, native ID
`7db5a3d55a55`, web ID `77600b108b51`. All 96 files / 179,884,306 bytes match the
runtime output (`artifacts/ptz-write-package-verification-20261004.json`). This
supersedes the earlier acknowledgement package for deployment testing. The build
has no C++/linker warnings and no dependency installation; third-party web
warnings remain visible. A fresh full vcpkg inventory confirms all 3,078 files /
1,830,711,343 bytes match the previous inventory with zero additions, removals or
content changes (`artifacts/dependency-ptz-write-after-20261004.json`).

No package was deployed, merged to main or pushed. Physical movement/Stop latency
and real firmware preset-write behavior are still unverified and require permission.

### Baichuan command acknowledgements (2026-10-04)

The connected Reolink path previously returned success after writing command 18
or 19 to its socket. Its reader discarded empty bodies, including header-only
acknowledgements. It now registers a bounded private ticket before writing and
waits up to one second after releasing socket-write ownership. Only a modern
reply matching both command and the entire 32-bit message/channel identity can
complete it; statuses 200, 201 and 300 succeed. Other statuses, timeout, write
failure, disconnect and Stop fail closed. Disconnect/Stop cancel pending tickets
and wake waiters. Duplicate, unmatched and late replies are ignored. The tracker
is private to the PIMPL; the exported class remains pointer-sized.

Reference for matched message replies and accepted status codes:
https://github.com/starkillerOG/reolink_aio/blob/master/reolink_aio/baichuan/base_protocol.py

`BaichuanCommandReplyTests` covers status handling, complete-before-wait,
command/channel/low/high message mismatch, legacy rejection, timeout, duplicate
completion, old-ticket cancellation, cancellation wake-up, 32-ticket bounds and
independent concurrent requests. `BaichuanPtzProbe` is a consumer of the actual
exported DLL, hardcoded to loopback and synthetic credentials. Run its peer with:

```powershell
$env:WITNESS_BAICHUAN_PTZ_PROBE = 'X:/Programming/Witness/build-vs2026/bin/RelWithDebInfo/BaichuanPtzProbe.exe'
C:/Python314/python.exe Tools/Tests/test_baichuan_ptz_transport.py -v
```

Five test methods / 19 TCP command cases pass: movement, Stop and preset accepted
statuses; header-only and nonempty replies; rejected statuses; all identity
mismatches; legacy replies; silent timeout; disconnect and explicit Stop
cancellation; and a motion/people event interleaved before acknowledgement.
The timeout cases take at least 900 ms and cancellation cases less than 700 ms
on this machine. Logs: `artifacts/baichuan-command-replies.log`,
`artifacts/baichuan-ptz-transport-final.log` (4.665 seconds), and
`artifacts/baichuan-ack-lifetime.log` (20 exported-DLL shutdown cycles). The
timestamp regression guard exits zero. The combined build has no C++/linker
warnings or dependency installation; existing third-party web warnings remain.

Package: `build-vs2026/deploy/baichuan-ack-c95df8b98965`, native ID
`c95df8b98965`, web ID `36cf9f72ad51`. The verifier confirms all 96 files /
180,004,626 bytes match runtime output and native/web identities agree with
their compiled artifacts (`artifacts/baichuan-ack-package-verification-20261004.json`).
It includes the preceding ordered improvements and contains no test executables,
database, authentication configuration or captured media.

Limits: the TCP fixture uses the existing empty-nonce/no-AES negotiation path.
It does not validate AES/TLS firmware, reconnect traffic, motor motion, physical
Stop latency or real preset movement. No physical camera commands, production
deployment, main merge or remote push were performed. The user's backlog edits
remain untouched.

### TLS ownership and connection-key checkpoint (2026-10-04)

The extended synthetic fixture negotiates a BC-XOR nonce/login, verifies the
derived credential hashes, decrypts outgoing AES-128-CFB PTZ commands, sends an
encrypted motion/people event, and exercises header-only/nonempty replies. It
uses the bundled OpenSSL CLI with synthetic values, not real camera secrets.
TLS cases use a temporary self-signed loopback certificate that is removed when
the fixture exits. Plain negotiation is deliberately rejected to exercise the
client's existing immediate TLS-upgrade path.

Before the ownership change, ten TLS event/shutdown runs reproduced five process
crashes: four access violations (`0xc0000005`) and one heap-corruption exit
(`0xc0000374`), recorded in `artifacts/baichuan-tls-repeat-before.log`. The reader,
PTZ caller and Stop path could concurrently read/write/free the same SSL object.
OpenSSL requires applications to prevent concurrent use of one SSL object:
https://docs.openssl.org/master/man7/openssl-threads/

The worker now owns every socket/TLS read, write and cleanup call. PTZ callers
enqueue at most 32 requests; expired/cancelled tickets are discarded before
delivery. Nonblocking TLS record I/O and 50 ms socket polling allow that owner
to service outbound PTZ while awaiting push events. WANT_READ/WANT_WRITE retries
preserve pending buffer arguments; pending read writes are retried before queued
controls. Stop cancels acknowledgements, interrupts the published socket and
joins its owner; only that owner closes the socket and frees TLS. Socket
publication/shutdown/close are synchronized, and disconnected retry sleeps can
be interrupted by Stop. The exported class layout remains unchanged.

A second reconnect regression failed when an AES session was followed by an
empty-nonce session: old key state encrypted commands that should be plaintext
(`artifacts/baichuan-key-transition-before.log`). Authenticate now clears the
previous session's key before negotiating. Reconnect tests verify both fresh-AES
and AES-to-empty-nonce sessions, injecting a stale reply with the opposite status
before the real reply to prove it cannot complete the new command.

Final evidence: eight methods / 30 synthetic transport cases pass in 18.699 s
(`artifacts/baichuan-ownership-key-final-transport.log`). Ten additional repetitions
of the TLS event, pending-Stop cancellation and disconnect cases (30 cases) pass
in 7.496 s (`artifacts/baichuan-ownership-key-tls-repeat.log`). Reply-state tests,
20 exported-DLL shutdown cycles and the timestamp guard exit zero. The combined
runtime build has no C++/linker warnings or dependency installation; third-party
web warnings remain visible. This is focused synthetic coverage, not a claim
of production stability, network-fault coverage or physical motor behavior.

Verified package: `build-vs2026/deploy/baichuan-ownership-e257df084b8f`, native ID
`e257df084b8f`, web ID `2bd87297e056`, 96 files / 180,083,986 bytes. Report:
`artifacts/baichuan-ownership-package-verification-20261004.json`. It supersedes
the prior package for testing and contains no fixture certificates, test
executables, databases or captured media. No production deployment, main merge
or remote push occurred. The user's backlog edits are untouched.

The user has now authorized brief low-speed move-and-Stop checks on cameras 10
and 12. No physical movement has yet been performed at this checkpoint; firmware
acknowledgements, actual movement and physical Stop latency remain unverified.
