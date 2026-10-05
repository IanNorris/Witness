# Unified Performance and Health Diagnostics

## Purpose

Witness should provide one administrator view and one export that correlate
server, camera pipeline, delivery, and current-browser playback evidence. The
primary goal is to answer *where* a fault began without repeatedly collecting
unrelated logs. The design is based on incidents already encountered: Tapo
stream corruption, non-monotonic timestamps, malformed or damaged fMP4,
simultaneous reconnects, processing backlog, browser decoder pressure,
adaptive Main/Preview selection, stale web bundles, startup maintenance, and
repeated FFmpeg errors.

The live dashboard must use cheap summary snapshots. Detailed packet,
fragment, log, and media evidence remains an on-demand incident export and
must never be polled as dashboard data.

## Correctness prerequisites

1. Publish thread-safe input, worker, processing, and stream snapshots. HTTP
   handlers must hold owning references while serializing, and counters must
   not be copied while another thread mutates them.
2. Scope FFmpeg diagnostics to the operation that produced them. In
   particular, `av_read_frame`, timestamp normalization, mux writes, and decode
   must not share an `input` scope. Errors emitted on an unattributed FFmpeg
   worker thread remain `unattributed` rather than being assigned to a camera.
3. Track Main and Preview/Sub as separate stream instances. Every observation
   carries camera ID, tier, worker epoch, connection epoch, init generation,
   and counter epoch.
4. Bound diagnostic storage by both item count and approximate bytes. Routine
   samples must not evict the incident ring.
5. Use `null` plus a reason for unavailable evidence. Zero must mean a measured
   zero.

## Summary API

`GET /debug/health` is administrator-only and returns a cached snapshot. A
dedicated sampler collects cheap gauges and cumulative counters every two
seconds; the request path does not walk packet traces or read log files.

Top-level fields:

- `schemaVersion`, `instanceId`, Git/build identity, web build hash, UTC and
  monotonic sample times, sample sequence, age, window, and uptime.
- Host CPU and memory capacity and use, Witness process CPU/private memory,
  logical processor count, maintenance state, and global worker occupancy.
- One entry per camera containing processing data and separate Main/Preview
  stream entries.
- A `coverage` object saying which evidence is available and why missing
  evidence (for example RTP sequence counters) cannot be reported.

Cumulative counters are valid only within a matching `counterEpoch`. Consumers
calculate rates from two samples with the same server instance, worker,
connection, and counter epochs. Identifiers and 64-bit hashes that are unsafe
as JavaScript numbers are strings.

For timed operations expose count, sum, and maximum with explicit units.
Percentiles require bounded histograms; lifetime maxima must not be labelled as
recent percentiles.

## Evidence by stage

### RTSP transport and demux

- Requested and effective transport/options where FFmpeg exposes them.
- Connection epoch, state, reconnect totals/reasons, read results, successful
  packet age, packet/byte totals by track, corrupt flags, missing/regressing
  DTS/PTS, arrival jitter, keyframe interval, and scoped warning/error clusters.
- Assign an ingress access-unit sequence immediately after `av_read_frame`
  succeeds, before any consumer can alter the packet. Record flags, timebase,
  timestamps, length, and an optional bounded-cost payload hash.
- RTP loss/reordering is reported only when it is genuinely observable. It
  cannot be reconstructed reliably from post-demux `AVPacket` data.

### Timestamp policy and normalization

- Accepted, startup-excluded, dropped, synthesized, repaired, and saturated
  packet counts separated by reason.
- Source/output timestamps, nominal and advertised durations, phase error,
  applied correction, policy state/transitions, and audio/video skew.

### Mux

- Packets submitted/rejected and FFmpeg result, init generation, fragment
  identity/length/hash, structural validation, and emitted sample/timeline
  validation where available.
- Valid `moof`/`mdat` structure alone is not evidence that sample tables or the
  encoded payload are valid.

### HTTP and WebSocket delivery

- Subscribers per tier; enqueue, send-complete, failure, and byte counters;
  backlog bytes/oldest age; obsolete-generation drops; and correlated HTTP
  segment or WebSocket generation/segment/part identity.
- A queued asynchronous send is not reported as client receipt.

### Browser transport, MSE, decode, and presentation

- Browser session/tab/player identity, build hash, selected tier and selection
  reason, requested physical viewport/DPR, codec, and player generation.
- Received fragment identity/length/hash, metadata mismatches,
  obsolete-generation drops, append queue age and outcomes, buffered ranges,
  discontinuities, codec reinitializations, playback seeks/rate, stalls,
  presentation progress, dropped/corrupted-frame deltas, render suppression,
  visibility, and suspension gaps.
- The last appended fragment and presentation position are separate facts.
- `MediaCapabilities` support/smooth/power-efficient results are capability
  hints. Actual hardware backend and available decoder sessions remain unknown
  unless an authoritative provider supplies them.

### Processing

- Ingress, started, completed, and coalesced frames, with optional-AI
  coalescing separate.
- Essential/AI queue depths, oldest age, active job age, worker occupancy,
  reservations, and queue-wait distributions.
- Each filter reports activation count, processed-frame count, total time,
  mean per activation, and amortized mean per pipeline-ingress frame.
- Scaling/JPEG work records a reason such as clip, preview, detection, or
  other. Decode coverage is identified as passthrough, keyframes-only,
  sampled, or full.
- Overlapping filter wall times are not summed and presented as CPU use.
- The existing stream `DecoderTimeTotal` is residual wall time after subtracting
  read and selected output intervals; it includes non-decode work and must not
  be presented as authoritative decoder CPU without replacement instrumentation.

## Health model

Health is `healthy`, `warning`, `critical`, `unknown`, or `disabled`, evaluated
independently for ingest, processing, delivery, and presentation. Overall
health is the worst required stage. Each reason contains a stable code, stage,
severity, first/last seen monotonic times, observation, threshold, unit,
window, evidence quality, and incident ID.

Initial configurable rules:

- Oldest essential work above 250 ms for 10 seconds is a warning; above one
  second for five seconds is critical.
- While expected to run, no ingest for three expected frame intervals (at
  least three seconds) is a warning; the configured read timeout is critical.
- Three reconnects in five minutes is a warning. Simultaneous reconnects are
  grouped as one incident but are not automatically assigned a common cause.
- Invalid fragment structure or a transport integrity mismatch creates an
  immediate stream/client incident.
- More than 5% dropped video frames divided by total video frames over ten
  seconds, with at least 100 total frames and a visible expected-to-play client,
  is a warning.
- Host CPU above 90% for 30 seconds is resource pressure, not proof of hardware
  decoder exhaustion.

Recovery uses lower thresholds sustained for 15 seconds. Startup, maintenance,
intentional pause, hidden tabs, inactive streams, and expected optional-AI
coalescing are explicit states rather than false alarms. Toasts occur once per
incident transition, group related cameras, support dismissal for that
incident, and retain evidence after dismissal.

## Retention and export

- Server: sample every two seconds, retain 15 minutes, plus one-minute
  aggregates for 24 hours and a separate bounded incident ring. Initial global
  budget: 32 MiB with overflow counters.
- Current browser: every two seconds while visible and ten seconds while
  hidden; retain ten minutes plus bounded important events, including disposed
  player instances.
- The browser report worker uploads bounded, authenticated summaries without
  raw media to `/debug/client-health`. An administrator export combines stored
  cross-client history with the current browser's detailed local evidence.
  Missing or stale reports are distinguished from live browser replies.
- JSON contains the complete correlated evidence, collection bounds, clock
  offset/uncertainty, versions, effective policies, selected detailed traces,
  and missing-data reasons. CSV/TSV contains the copyable per-camera table and
  protects against spreadsheet formula injection.
- Serialization copies bounded snapshots and never holds the global context,
  stream, or processing locks while producing the response.

### Implemented cross-client history and validation

The separate `client-health.db` worker retains at most 24 hours, 8 MiB of
payload/identity data, 2,048 samples, 64 sessions globally, eight sessions per
authenticated user and 120 samples per session. The submission queue holds at
most 64 samples. A report is at most 64 KiB; per-session and per-user rate limits
also apply. SQLite storage is capped independently. This is a recent diagnostic
ring, not a permanent activity audit trail. Browsers normally report every
15 seconds, including hidden tabs subject to browser throttling, with a
five-second request deadline and retry. Silent/disconnected clients retain
their last submitted evidence until retention or capacity eviction.

POST requires a normal authenticated session and its CSRF token. GET requires
administrator privileges. There is no Debug/loopback bypass for this endpoint.
User UID, username, receive time and sequence come from the server; supplied
ownership fields are ignored. Raw error messages, credentials and arbitrary
nested event payloads are not stored. The persistent local browser ID and
per-page session ID identify reports, not authentication principals.

`Tools/Tests/ClientHealthHttpFixture.cpp` and `test_client_health_http.py` add an
opt-in actual HTTP integration test. The fixture binds only to `127.0.0.1` on an
ephemeral port, uses temporary databases, and calls the production `CrowAuth`
implementation and authentication SQL extracted from `Database.cpp`. It starts
no cameras and does not disable authentication. The test covers absent cookies,
wrong cookie-port namespace, normal versus admin privileges, disabled/missing
users, wrong/missing CSRF, accepted and rate-limited POSTs, forged ownership,
cross-user export, session revocation, and durable history across process restart.
The administrator can export a revoked client's retained samples but that client
can no longer submit reports. The real-cookie HTTP test passes.

Build the explicit `ClientHealthHttpFixture` target, set
`WITNESS_HEALTH_HTTP_FIXTURE` to its executable path, and run
`python -m unittest discover -s Tools/Tests -p test_client_health_http.py -v`.
Without the environment variable it skips rather than contacting a live server.
The fixture test does not validate the login/password endpoint, HTTPS cookie
transport, the full `CrowListener` registration, or actual multi-browser UI
reporting; those remain distinct integration/production validation concerns.

## Tapo corruption acceptance scenario

Current traces start inside `LiveOutputStream`, after demux, and cannot prove
what entered Witness. A matching browser fragment hash proves delivery of the
muxed bytes, not validity of the original access unit. `AV_PKT_FLAG_CORRUPT ==
0`, successful MSE append, and zero browser-reported corrupted frames also do
not exclude decoder concealment.

For a captured incident, Witness must correlate input access units, codec
extradata and timestamps with normalization decisions, mux init/fragments,
delivery identity, and browser evidence. An opt-in bounded media capture can be
replayed through a software decoder. Comparisons account for Annex-B versus
length-prefixed representation and parameter-set transformations. This can
separate corruption already present at the demux boundary from corruption
introduced later, but distinguishing camera firmware from Wi-Fi/RTSP transport
still requires genuine transport-level loss/reordering evidence.

## Delivery order

1. Thread-safe snapshots, bounded client events, stage-specific FFmpeg scopes,
   identities, and Main/Preview coverage.
2. Cheap health endpoint exposing existing build, host, queue, and stream
   metrics with explicit missing evidence.
3. Operations page with a host strip, sortable camera table, expandable stage
   reasons/timeline, and copy/export actions.
4. Bounded server history, health hysteresis/incidents, grouped FFmpeg error
   counters, and unhealthy-camera toasts.
5. Correlated on-demand media evidence and deterministic replay tooling.

Automatic remediation, numeric health scores, permanent raw-media retention,
universal validation decoding, and claims about hardware-session capacity are
out of scope until supported by measurements.

## Test gates

- Fixed-clock epoch and health tests cover resets, reconnects, hysteresis,
  recovery, startup grace, hidden clients, and grouped incidents.
- H.264/H.265 fixtures cover truncated and reordered access units,
  missing/regressing timestamps, malformed sample tables, valid-container
  damaged payload, corrupt random-access frames, and audio drift. Tests require
  the correct stage attribution or an explicit insufficient-coverage result.
- Flipping one WebSocket payload byte identifies delivery integrity without
  blaming ingest.
- Main/Preview switches and codec remounts retain correct histories and reject
  stale callbacks.
- Shutdown and HTTP polling stress detects races, deadlocks, and lifetime bugs.
- Ten thousand identical FFmpeg errors retain exact totals while bounded
  storage/logging remains bounded.
- Exports contain no credentials or inaccessible cameras and remain valid with
  partial data-source failures.
- With 16 representative streams for 30 minutes, diagnostics add less than one
  percentage point of host CPU, stay within budget, increase processing p95 by
  less than 5%, and keep summary HTTP p95 below 100 ms.

### Browser-history validation checkpoint (4 October)

The existing native limit/privacy tests are now explicit `EXCLUDE_FROM_ALL`
targets, rather than relying on ad-hoc compiler commands:

```powershell
cmake --build build-vs2026 --config RelWithDebInfo --target ClientHealthPayloadTests ClientHealthHistoryTests ClientHealthEndpointTests ClientHealthHttpFixture
```

All three focused tests passed against the current history implementation. They
cover payload allowlisting and server-owned identity; invalid/oversized input;
per-session/principal rate limits; 120 samples/session, eight sessions/user,
64 global sessions, 2,048 records and 8 MiB retained sample-content bounds;
24-hour expiry, durable reload and storage initialization failure. The endpoint
test checks permission/CSRF rejection, method/body limits and administrator
export. The separately rerun loopback HTTP fixture verifies actual production
cookie authentication and persistence through a fixture process restart.

The near-limit history export measured 16 ms and 9,393,620 serialized bytes in
this one local run. Serialized export includes both latest-session summaries
and history, so its size is not the retained-content byte bound. This is not a
p95 measurement, a concurrent-server benchmark, or the 16-stream/30-minute
overhead gate above. Real-browser multi-client reporting, Linux execution and
full production-load validation remain unproven by these isolated tests.

### Real-browser reporter checkpoint (4 October)

An optional loopback-only harness now bundles the actual `useClientHealth` and
`useApi` modules. Its native fixture executes production cookie/CSRF checks with
disposable synthetic administrator/session rows and a private temporary history
database. It serves only a fixed test page and the generated reporter module;
there are no camera workers, production credentials, authentication bypass or
media routes. The optional module is bounded to 1 MiB and must reside directly
in the fixture's temporary directory. Browser-mode lifetime is bounded to five
minutes, with an independent launcher deadline.

```powershell
node web-ui/tests/fixtures/buildClientHealthBrowser.mjs
python Tools/Tests/run_client_health_browser_fixture.py --fixture build-vs2026/bin/RelWithDebInfo/ClientHealthHttpFixture.exe --module artifacts/client-health-browser-reporter.js
```

Using the returned ephemeral port, open `/fixture/browser/one` and
`/fixture/browser/two`. The computer-use check observed two real browser tabs
with the same persisted browser ID and distinct session IDs, automatic samples
from their synthetic players, and server-owned `fixture-admin` identity. Reload
retained the browser ID while generating a new session ID. After closing the
first tab, the second tab's authenticated export still contained all three
session records, six durable historical samples and zero pending/rejected/storage
failure counts. Stop reporting remained independent of reading retained history.
Screenshot: `artifacts/client-health-browser-retained.png`.

Both temporary browser tabs and the listener were closed. The original real
cookie/CSRF, disabled/deleted user, cross-principal and process-restart test
passed again after the harness extension. This test exercises real browser
timers, localStorage, fetch and cookies, but synthetic player snapshots rather
than real MSE playback. The in-app browser reported both tabs visible, so hidden
tab throttling and independently profiled browsers remain unverified. It is not
the 16-stream/30-minute performance gate above.

### Concurrent HTTP history-load checkpoint (4 October)

`Tools/Tests/benchmark_client_health_http.py` launches the native cookie/CSRF
fixture twice with disposable databases and two Crow workers: empty history,
then 126 near-maximum-size records across 16 stable synthetic browser sessions.
The latter starts with 9,321,687 serialized export bytes (history plus latest
session summaries). Deliberate JSON padding models the storage/serialization
bound; it is not representative real-player content. Synthetic reporters reuse
seeded identities so large records remain retained, and an explicit final
assertion requires more than 8 MB of sample content. The first exploratory run
rotated identities and evicted the large records; its measurements do not prove
near-limit performance and are superseded by the retained-identity run.

```powershell
python Tools/Tests/benchmark_client_health_http.py --fixture build-vs2026/bin/RelWithDebInfo/ClientHealthHttpFixture.exe --output artifacts/health-http-load-retained-20261004.json
```

Two concurrent export clients, one authenticated tiny-response probe every
100 ms and one synthetic 16-player report every second run for 30 seconds in
each case. Timings cover complete client HTTP responses, including transport
and body reads, not isolated handler CPU. Nearest-rank quantiles in milliseconds:

| Case | Operation | Samples | P50 | P95 | P99 | Maximum |
|---|---|---:|---:|---:|---:|---:|
| Initially empty | Export | 1903 | 2.097 | 25.971 | 26.689 | 27.727 |
| Initially empty | Tiny authenticated probe | 266 | 14.008 | 24.085 | 25.982 | 26.865 |
| Initially empty | Report POST | 30 | 11.620 | 25.541 | 25.676 | 25.676 |
| Near-limit retained | Export | 870 | 47.356 | 71.613 | 88.224 | 100.703 |
| Near-limit retained | Tiny authenticated probe | 231 | 28.697 | 57.385 | 77.315 | 91.719 |
| Near-limit retained | Report POST | 30 | 26.022 | 79.559 | 91.555 | 91.555 |

Near-limit history ends at 156 durable rows with no pending writes or storage
failures. Baseline tiny-probe P95 before concurrent traffic is 25.133 ms (empty)
and 15.714 ms (loaded), showing local scheduling variability. The fixture-only
probe uses production administrator authentication; regressions reject anonymous
and ordinary-user calls. Cookie/CSRF, deleted/disabled owner and durable restart
tests also pass after the fixture extension. The isolated fixture builds without
compiler warnings and performs no dependency installation.

This demonstrates bounded HTTP responsiveness in this synthetic run, not the
production summary endpoint, camera processing overhead, sustained browser
reporting CPU or the 16-representative-stream/30-minute gate. No recording DB,
authentication setting, camera worker or production runtime route is changed.

### Shared host/process CPU measurement checkpoint (4 October)

The earlier health API reported CPU coverage as not implemented even though the
terminal independently sampled host CPU. Both now use one process-wide sampler.
It reuses a cached result for 500 ms, so concurrent terminal/HTTP consumers do
not consume each other's tiny counter-delta windows. Collection remains on
demand: this adds no background thread or persistent storage. It does not cache
the entire health response or implement the future periodic summary sampler.

Windows host CPU is busy-time percentage from GetSystemTimes deltas; kernel
time includes idle, which is subtracted once. Process CPU uses GetProcessTimes
kernel/user deltas in 100 ns units divided by monotonic elapsed time and total
logical CPU capacity. Thus 100% process CPU means the full host, not one core.
References: [GetSystemTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes)
and [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes).
Host CPU above 64 logical processors remains unavailable because GetSystemTimes
is processor-group scoped there. Process CPU can still be collected. Non-Windows
collection remains an explicit `platformNotImplemented` stub; Linux execution
was not tested in this checkpoint.

Health `server.host` adds optional numeric `cpuPercent` and `processCpuPercent`,
`cpuSampleWindowMs`, `cpuSampleAgeMs`, unavailable reasons, and an explicit
process-normalization description. Warmup, counter/clock resets, missing data,
invalid deltas and capacity changes produce null, not a guessed zero. Measured
zero remains numeric zero. Coverage carries the current availability reason.
The Health cards show host and Witness CPU; the latter is labelled as total host
capacity. Older reports remain readable, and warmup/reset/unsupported values
have distinct human-readable labels. Existing endpoint authentication and
client-history retention policies are unchanged.

`CpuUsageTests` is an opt-in native target. It verifies known 80% host / 25%
process arithmetic, measured zero, unavailable/reset/invalid deltas, partial
coverage, sample windows, exact production health JSON serialization, and 8,000
concurrent cache reads without extra OS sampling. Its real Windows producer
smoke test collected a 564.353 ms window (host 20.3704%, idle test process 0%).
That is the test executable's CPU, not a loaded Witness-server measurement.
Log: `artifacts/unified-cpu-final-native-tests.log`. PTZ graph/lease configuration
and timestamp regression guards also exit zero. All 46 client tests pass,
including two CPU display/legacy/null-versus-zero cases. Vue type checking and
the combined build pass without C++/linker warnings or dependency installation;
existing third-party web warnings remain. The new card text was not separately
visually inspected in a browser.

Verified package: `build-vs2026/deploy/unified-cpu-ce1cb1924b1e`, native ID
`ce1cb1924b1e`, web ID `baeb86a8a3de`, 96 files / 180,175,836 bytes. Report:
`artifacts/unified-cpu-package-verification-20261004.json`. It includes the prior
ordered improvements and excludes probes, databases, certificates and captures.
No production deployment, main merge, remote push or camera movement occurred.
The user's backlog edits are unchanged.

This supplies a missing measurement prerequisite. It does not establish the
16-representative-stream/30-minute diagnostics overhead gate, processing P95
overhead, summary HTTP P95 under production load or managed dependency migration.

### Browser-history snapshot contention checkpoint (4 October)

Inspection found that `GetSnapshot()` copied up to 8 MiB of retained payload
strings while holding the same mutex used by submissions and the persistence
worker. Stored rows are now immutable shared objects. Snapshot collection copies
at most 2,048 references under the mutex, then releases it before allocating and
copying payload strings. Exports still return caller-owned snapshots, with the
same receive-time filtering, ordering, identity, durability and JSON schema.
No database work was added to HTTP callers. Authentication is unchanged.

References keep retired rows alive until an in-flight snapshot has finished
copying them. The retained store and pending queue keep their existing limits;
the 8 MiB limit is not a cap on all concurrent HTTP response/serialization
allocations, and was not one before this change either. Collection still has a
bounded row scan and reference-vector allocation under the lock; it is not
lock-free, nor is this the planned periodic cached summary sampler.

The native persistence/bounds test passes with two concurrent readers checking
32 snapshots plus JSON exports while new near-limit rows force pruning.
It also proves that changing a returned snapshot cannot modify stored payloads.
Existing byte/session/record bounds, retention, reload and storage-failure cases
remain enabled. Logs: `artifacts/history-snapshot-regressions-final.log` and
`artifacts/history-snapshot-endpoint-regressions.log`. The actual Crow HTTP
cookie, CSRF, cross-user and restart regression also passes:
`artifacts/history-snapshot-http-auth-regressions.log`.

The combined legacy-dependency runtime builds without native compiler/linker
warnings or package installation. The preliminary HTTP benchmark overlapped a
fixture build, so its before/after timings must not be presented as a controlled
speedup. This checkpoint does not establish the full 16-stream/30-minute gate.

An updated run after builds/tests had stopped used two Crow workers, two export
floods and one synthetic 16-player report per second, for 30 seconds each with
empty and near-limit retained history. The populated run completed 896 exports
(P95 69.290 ms, P99 76.945 ms, maximum 92.705 ms), 233 ping requests
(P95 57.801 ms) and 30 telemetry POSTs (P95 57.530 ms), with all 156 final rows
durable. Report: `artifacts/history-snapshot-after-20261004.json`. Export P95 is
similar to the preliminary run; this change removes a payload-copy critical
section, not the serialization/HTTP-worker cost. These synthetic measurements
are not production summary latency or evidence of improved camera playback.

### Ordered goal audit (5 October)

The five functional work pieces are implemented, but the full-runtime
validation below remains open. The active goal is not being marked complete
from component tests or package identity checks.

| Goal item | Current authoritative evidence | Remaining validation limit |
|---|---|---|
| Tapo cadence/stalls | Concurrent 900-second camera 8/9 demux/mux runs, paired payload/decode checks and saved-output real-browser MSE replay. InputStream.cpp and LiveOutputStream.cpp have no diff from the measured e8e1f36 implementation. | Replay does not reproduce live server delivery, original network jitter or the complete processing workload. |
| Bounded authenticated browser history | Persistence/retention/byte/session bounds and concurrent snapshot/pruning tests; actual Crow cookie, CSRF, cross-user and restart tests; bounded HTTP export benchmark. | Independent browser profiles and a complete live-server workload have not been tested together. |
| PTZ and explicit fullscreen selection | Final 46-test client suite; production SQL/lease regressions; HTTP/Baichuan acknowledgement, TLS ownership and exported-DLL tests; real cameras 10/12 acknowledged brief Move and Stop. | Real firmware replies are not measured motor stopping time; full production HTTP-to-camera routing and concurrent reconfiguration are not proved by the direct probes. |
| Dashboard modes/layouts/activity placement | Priority/manual/automatic-slot geometry regressions and built-dashboard real-MSE slot transitions; five players remain primed through promotion/fallback. | Five synthetic IDs replayed one saved source, not five independent live cameras; hardware-capacity and hidden-tab behaviour remain outside that run. |
| Measured build/PIMPL/dependency reuse | Compiler profiling, clean native warning results, cross-DLL shutdown tests, corrected separate pins and actual managed bootstrap/configure/build with every installed file unchanged. | A clean-machine source bootstrap and actual compiler-servicing A/B comparison were not performed. |

Final client suite: `artifacts/ordered-goal-final-ui-tests.log`, 46 passed,
zero failed/skipped. Detailed evidence and limitations remain in
`Tools/TAPO_PACKET_CAPTURE_FINDINGS.md`, `Tools/PTZ_CONTROL_VALIDATION.md`,
`Tools/DASHBOARD_LAYOUT_VALIDATION.md` and `BUILDING.md`.

Current coherent managed package:
`build-vs2026-stable/deploy/managed-reuse-d53320113674`, native
`d53320113674`, web `c13b0dc19b82`, 94 files / 174,189,020 bytes;
`artifacts/managed-lf-package-verified.json`. No production deployment, main
merge or remote push has occurred. The user-owned backlog changes are unstaged.

The next full-load harness must run actual server camera workers, decode/mux,
essential/AI queues, HTTP/WebSockets and browser reporting, not only the existing
HTTP-history fixture or passthrough probe. Its data root, recording database,
history database, cache, logs and listener must be isolated before initialization;
the current GetConfigFilePath uses the machine-wide Windows directory, not the
process working directory. Changing cwd or copying the production database
alone is therefore insufficient. Startup cleanup must have no path to real
recordings, and authentication must remain enabled with fixture-owned sessions.

Use a declared H.264/HEVC source/resolution/frame-rate matrix for 16 streams,
explicit baseline/enabled phases of at least 30 minutes, and report decode,
queue, playback, CPU and HTTP evidence separately. Processing P95 requires
actual per-item distributions, not reconstruction from cumulative means.
Missing sources, unsupported decode, hidden-tab throttling, storage failures or
unfinished phases must be reported as incomplete rather than a passing gate.
No longer test has been started yet; there is no live job being waited on.

### Data-root isolation prerequisite (5 October)

The server now accepts an explicit `WITNESS_DATA_ROOT` process-environment
override, with default locations unchanged when absent. Configuration path
selection is a standalone, regression-tested module; invalid relative/volume
roots or escaping filenames are rejected before filesystem writes. Early log
initialization reports a clean exit instead of letting a malformed root produce
an uncaught exception. Windows environment values are copied with the secure
CRT API rather than retaining mutable environment pointers.

Native selection/containment tests and a real `/createdb` subprocess test pass.
The subprocess creates only a fresh private database/log directory and verifies
that an invalid relative override fails before accessing that database. It does
not start server workers, HTTP or camera connections. No global configuration,
authentication or existing media is changed. See `BUILDING.md` for exact scope:
cache/TLS/media-reference isolation still has to be configured and checked by
the full-load harness. The 16-stream test has not been started by this change.

### Full-server lifecycle smoke: failing gate (2026-10-05)

`Tools/Tests/test_full_server_isolation.py` is opt-in with the same
`WITNESS_TEST_DATA_ROOT_SERVER` and `WITNESS_TEST_DATA_ROOT_NATIVE_ID` variables.
It refuses an executable without the requested native identity/data-root support,
creates a fresh database/cache, seeds only a fixture-owned administrator session,
and starts the real server on numeric loopback in a hidden dedicated console.
Authentication remains enabled. It attempts two runs to validate durable browser
history across a graceful restart; it never reads a live database or camera URL.

The packaged native `119f84433c11` passed authenticated health, anonymous denial,
history-store readiness, POST acceptance and durable history export in the first
run. The initial POST briefly returned 503 before the asynchronous store was
ready; the harness now waits for storage availability explicitly. Two subsequent
runs reproduced exit `3221226505` (`0xC0000409`) after a targeted CTRL_BREAK
shutdown signal. Therefore restart validation is **not passed**. The exact
shutdown fault/stack has not yet been established, and no fix is claimed.

Evidence: `artifacts/full-server-isolation-smoke.log` and retained private-fixture
server log `artifacts/full-server-isolation-failed-server.log`. Fresh startup also
emitted missing face-table preparation errors; investigate their ordering before
treating fresh-database initialization as warning-clean. Test processes ended and
their private database/cache directories were removed. No cameras were connected
or moved. This zero-camera lifecycle smoke is not the 16-stream performance gate;
the 30-minute load phases, CPU and processing/HTTP P95 evidence remain outstanding.

### Dump-confirmed shutdown race and fix (2026-10-05)

The user-provided `WitnessServer.exe_261005_004649.dmp` loads matching private
symbols from the `119f84433c11` package. The exception is `FAST_FAIL_FATAL_APP_EXIT`
(subcode 7), not evidence of a stack-buffer overwrite: an uncaught C++ exception
from `std::thread::join` in `WorkerBase::Join` escapes `RequestShutdown` on the
console-handler thread. At the same instant, the main thread is in
`Shutdown -> ImageWorkers.clear -> WorkerBase destructor -> join`. This confirms
that main-thread destruction overlaps handler-thread worker joins. Stack evidence
is retained in `artifacts/full-server-shutdown-dump-stack.log`; the dump itself
remains in its original private location and is not packaged or committed.

The fix serializes console/service stop callbacks with main-thread teardown,
makes the run flag atomic, prevents repeated stop requests, and only publishes
the server to callbacks after initialization completes. Main-thread teardown waits
for the callback's stop/join sequence before releasing worker ownership. A stop
received during initialization is retained and processed after initialization;
the callback no longer operates on a partially initialized server. No join
exception is swallowed and no camera/HTTP authentication behavior is changed.

The previously failing full-server lifecycle regression now passes against native
`866a4420204d`, including authenticated diagnostics, anonymous rejection, durable
history export, clean shutdown, restart, and retained history. Additional repeated
run logs are `artifacts/shutdown-single-owner-repeat-*.log`. Service Control Manager
shutdown, interrupted initialization, repeated simultaneous control signals and
camera-loaded shutdown are not yet independently exercised by this zero-camera
test. Missing face-table preparation warnings remain a separate investigation.

Deployable package: `build-vs2026-stable/deploy/shutdown-order-866a4420204d`,
native `866a4420204d`, web `199371ef8283`, 94 files / 177,487,324 bytes, verified
byte-for-byte against the build runtime by
`artifacts/shutdown-order-package-verified.json`. Native build completed without
C++/linker warnings or dependency installation; frontend chunk-size advisories
remain. The full multi-stream performance gate and overall goal remain open.

### Camera-loaded lifecycle replay harness (2026-10-05)

The same full-server test now accepts `WITNESS_TEST_REPLAY_INPUTS`, a JSON array
of up to 16 existing local media filenames, and `WITNESS_TEST_REPLAY_SECONDS`
(10–3600 seconds per run; default 20). Without inputs it retains the zero-camera
lifecycle test. Each input gets its own numeric-loopback FFmpeg MPEG-TS HTTP
sender, paced with `-re`, looped and packet-copied. The fixture creates enabled
camera rows in its new database, leaves PTZ/substreams disabled, enables motion
vector extraction, and uses a valid high motion threshold (0.99) so this smoke
does not intentionally trigger clip/AI work. Replay processes are stopped by
their owned process handles and the private database/cache is removed afterwards.

Readiness requires packets on every camera. During each run the test samples the
authenticated real health handler once per second, then asserts both accepted
packets and completed processing jobs advanced for every camera. It stops the
server while its camera workers are active, restarts fresh replay senders and
the same isolated server database, and verifies persisted browser history.
HTTP samples and actual host/process CPU readings are retained in
`artifacts/full-server-replay-run-{0,1}.json`; these are not processing latency
distributions or an instrumentation baseline comparison.

The first successful 16-input run pair used eight copies of saved Tapo H.264
1280×720 media and eight copies of a synthetic HEVC 160×96 fixture. Both phases
were 20 seconds, all 16 cameras stayed connected and completed processing jobs
advanced; sampled cumulative packet drops and mux errors were zero. This proves
camera-loaded lifecycle behavior only: it is not 16 independent cameras, not
representative HEVC resolution, not the original RTSP/arrival-clock network path,
not browser playback, and not the required 30-minute baseline/enabled gate.
The initial harness failures were an invalid threshold of 1.0 and an incorrect
health JSON nesting assumption; no product fix was made to accommodate them.

The deployable server remains the verified `shutdown-order-866a4420204d`
package above; this work piece changes the test harness only. The overall goal
stays active until the remaining representative workload/browser/performance
requirements have actual evidence.

Finalized harness regression: `artifacts/camera-loaded-server-regression-final.log`
passed in 68.117 seconds with both phases and the explicit processing-advancement
assertions enabled. All fixture-owned server/sender processes ended. The user's
unrelated `BACKLOG.md` edits are preserved and excluded from this commit.

### Real browser coupled to the isolated full server (2026-10-05)

Optional `WITNESS_TEST_BROWSER_HOLD_SECONDS` (60–3600; default 0) holds a
single camera-loaded run open and prints only its loopback URL. This mode copies
the deployment into the fixture's private runtime, and changes only that copy's
HTML to bootstrap a fixture-owned session cookie and MSE/layout preferences.
Compiled assets and native binaries are unchanged; the verified package/build
are not modified. Authentication stays enforced, including anonymous denial.
Fixture-only camera/user/group mappings authorize the disposable administrator
to the replay cameras. Session tokens are not printed or written to evidence;
the private runtime/database and bootstrap cookie lose their server when the
owned processes stop and the temporary directory is removed. This test bootstrap
is not a production login or security change.

A 180-second server run used sixteen independently paced HTTP senders of the
saved Tapo H.264 1280×720 recording. The actual compiled dashboard was opened in
the in-app browser through computer-use, showing all 16 real MSE players. Two
DOM observations showed all 16 unpaused, readyState 4, error null, and media
clocks advancing from approximately 27 to 98 seconds. Selecting the generated
`16 small` preset succeeded without replacing the playback pipeline. Screenshot:
`artifacts/isolated-full-server-dashboard-16.jpg`.

A second browser tab opened the real health monitor. It collected persisted
records from the dashboard and showed all 16 players with zero restarts, stalls,
errors and corrupt frames, while nonzero browser dropped-frame counts remained
visible (0–3 per player in that observation). After the dashboard tab closed,
refreshing the monitor showed no responding live dashboard but retained all 16
players from its last heartbeat. Evidence:
`artifacts/isolated-browser-disconnected-health.txt` and the fixture's final
`artifacts/full-server-browser-history.json`. Both tabs share one browser profile;
this does not prove independent-profile behavior. Playback was observed for
roughly 105 seconds before intentionally closing the tab, not for the complete
180-second server phase.

This establishes that the real server/auth/WebSocket/MSE/history path can be
tested in isolation. It is still not the representative 30-minute gate: repeated
720p H.264 media is not a mixed H.264/HEVC resolution matrix, no AI/clip workload
was intentionally triggered, no baseline/enabled comparison was made, and queue
means/maxima are not processing P95 distributions. The user-provided Reolink
samples advertise r_frame_rate 90000/1 and previously reported pacing/corruption
problems; they require timing/payload qualification before use as clean workload
inputs. No conclusion about diagnostic overhead or production stability follows
from this run.

Run result: `artifacts/isolated-browser-load-run.log` passed in 203.135 seconds
(180 seconds sampled after readiness plus initialization/cleanup). Final export
contained nine history samples, durableThroughSequence 9, storageFailures 0.
The private server and all sixteen sender processes stopped cleanly; both test
browser tabs were closed. This branch modifies only the validation harness/docs
and continues to use the verified deployable shutdown-fix package above.

### Bounded completed-task processing work percentiles (2026-10-05)

Inspection found `FilterStat_Process_Total` exposed to consumers but never
populated by a processing scope. The queue worker now measures steady-clock
nanoseconds spent within each filter `DoWork` call and accumulates them in the
task's existing frame stats. Finalized tasks with measured work feed a private
1024-sample ring per source; admission is O(1). `GetStats` copies the ring under
the existing locks, releases them, then sorts only the copy. No sorting or window
allocation occurs in `AddFrame`. The public DLL snapshot exposes only scalar
results, not the ring or its STL layout.

The authenticated health response now includes `processing.workLatency` with
P50/P95/P99/max milliseconds, sample count, samples since reset, capacity,
nearest-rank method, explicit scope and a profiling-reset epoch. An empty window
returns null percentiles rather than claiming zero latency. Reset clears the
window and advances its epoch while preserving live queue gauges. The window is
the last 1024 finalized tasks, not a fixed elapsed-time interval or lifetime
distribution. Its storage bound is per source (8 KiB of sample values); no new
global camera-count limit is claimed.

This is **filter-stage wall work**, not thread CPU, input-to-result latency or
queue residence. Work can include blocking/DB/enqueue operations executed inside
the filter call. Input decode and time waiting for dispatch remain separate.
Existing finalization also counts optional-AI candidates whose essential work
finished but whose inference was coalesced away; their measured work is included,
not fabricated inference latency. Comparisons must use like-for-like workloads
and the same completion population. Tasks completing after a profiling reset
enter the new completion epoch even if an earlier stage ran before the reset.

`ProcessingLatencyWindowTests` passed empty/zero/negative cases, exact rank
selection, 1024-sample wraparound, copy isolation and reset/gauge preservation.
The real-server 16-replay test with `WITNESS_TEST_PROCESSING_LATENCY=1` passed both
phases and clean restart, positive bounded ordered percentiles for every camera,
and actual authenticated `/camera/admin_reset_stats` epoch/window resets.
Evidence: `artifacts/processing-latency-server-regression-final.log` (68.725 s),
`artifacts/processing-latency-15b2da5f9014-run-{0,1}.json`, and native test/build
logs. These short mixed 720p H.264/tiny HEVC inputs are not the representative
load gate. No CPU-overhead or less-than-5% P95-overhead claim is made yet.

Coherent deployable build:
`build-vs2026-stable/deploy/processing-latency-15b2da5f9014`, native
`15b2da5f9014`, web `e2d8fb683d50`, 94 files / 177,658,844 bytes. Package verifier
`artifacts/processing-latency-package-verified.json` confirms exact runtime
matches and excludes test binaries/private data. `SourceStats` gained scalar
fields, so consumers must deploy the rebuilt matching server/Camera DLL together;
this does not claim binary compatibility with an older consumer. Native build
has no C++/linker warnings or vcpkg installation; frontend chunk advisories remain.
All private-fixture processes stopped, and the user's backlog edits remain intact.

### Qualified mixed-resolution replay media (2026-10-05)

`Tools/Tests/prepare_replay_media.py` creates new video-only constant-cadence
workload copies of local H.264/HEVC media with an explicitly reported absence of
B-frames. It never edits the source. Publication requires strict decoding with
no error diagnostics, exact ordered SHA-256 equality of every encoded video
packet, and exact requested frame-rate metadata. Existing outputs are rejected;
same-volume hard-link publication also refuses to replace a concurrently created
output. Four regression tests cover stream guards, existing-output preservation,
decode/payload/cadence validation failures and publication races. They passed in
0.033 seconds. Fixture temporaries reside under repository artifacts.

Qualification of the original saved Reolink media distinguishes payload faults
from timing faults. `1789571245.mp4` decodes all 522 video packets/frames without
diagnostics, but actual DTS gaps range from approximately 6 to 590 ms (median
36 ms, P95 107 ms). Its advertised r_frame_rate 90000/1 alone is not proof of
bad media: the measured intervals establish uneven pacing. `1789602807.mp4`
and `1789601697.mp4` report respectively 209 and 248 strict decoder diagnostic
lines and fewer decoded frames than packets, despite decoder process exit code
zero. They are fault fixtures, not clean load inputs. Qualification evidence:
`artifacts/replay-qualification-<original filename>.json`.

Two payload-preserving copies passed strict decoding: 1920×1080 HEVC at 20 fps
(522 packets/frames, from `1789571245.mp4`) and 3840×2160 HEVC at 25 fps (968
packets/frames, from `Sound2_1789650188.mp4`). Evidence and generated media:
`artifacts/replay-qualified-hevc-1080p20.{json,mp4}` and
`artifacts/replay-qualified-hevc-2160p25.{json,mp4}`. These deliberately replace
the captured video clock and remove audio. They qualify decoder/load workloads,
not original camera pacing, original network transport, or A/V synchronization.
The saved Tapo workload is also an accepted-output capture, not a raw RTSP feed.

The actual full server then passed two 60-second sampled runs with eight 720p
H.264 senders, four qualified 1080p20 HEVC senders and four qualified 2160p25 HEVC
senders. Every camera advanced accepted packets and completed processing tasks;
all percentile windows and authenticated profiling resets passed. Both server
instances stopped gracefully and all sixteen fixture-owned senders were stopped.
Test duration including readiness/restart/cleanup was 149.113 seconds. Evidence:
`artifacts/mixed-qualified-server-preflight.log` and preserved
`artifacts/mixed-qualified-15b2da5f9014-run-{0,1}.json`.

Sixty health requests per run had measured end-to-end P95 25.13 / 25.96 ms.
Mean Witness process CPU was 23.56 / 23.61 percent of host logical CPU capacity;
final private memory was 853.50 / 857.68 MiB. These are observations, not overhead
comparisons. Final camera snapshots were connected with zero established drops
and no timestamp rejection. There was no browser attached in this preflight,
motion thresholds intentionally avoided AI/recording work, and the media clock
was normalized. Neither original network behavior, playback, production load,
nor the 30-minute baseline/enabled gate is established. No new native build was
needed: the test used coherent native 15b2da5f9014 / web e2d8fb683d50 unchanged.

### Mixed-codec real-browser preflight (2026-10-05)

The same 8×720p H.264, 4×1080p20 HEVC and 4×2160p25 HEVC matrix ran for a
240-second server sampling phase with the actual compiled dashboard attached.
Computer-use selected the generated `16 small` layout. Four ordered DOM
observations found sixteen native video elements (no JPEG fallback), all
unpaused, with their expected native resolutions and advancing clocks. An
observation near 200 seconds found one 4K element at readyState 2; other elements
were readyState 4. Error codes were null and no warning/error console logs were
captured. Evidence: `artifacts/mixed-qualified-browser-media-observations.json`
and `artifacts/mixed-qualified-browser-dashboard.jpg`.

This is not an unqualified smooth-playback pass. The actual health UI showed
brief `media:waiting` events, decreasing buffered headroom and presentation
drops, especially for 4K HEVC. In the final persisted heartbeat at roughly 210
seconds of player age, H.264 dropped-frame counts were zero, 1080p HEVC counts
were 31–53 out of approximately 4,146 frames, and 4K counts were 284–313 out of
approximately 4,990 frames. One 4K player reported readyState 2. Restart/stall
watchdog/error counters were zero, which does not exclude sub-watchdog-duration
waits or poor pacing. Input/server drops and browser presentation drops must
remain separate evidence. Delivery pacing of the looped workload must also be
audited independently before attributing this to production decode or MSE.

The real monitor showed retained server heartbeats as well as the live browser
snapshot. Final history had durableThroughSequence 16, pending 0, rejected 0 and
storageFailures 0. Preserved evidence:
`artifacts/mixed-browser-15b2da5f9014-history.json`,
`artifacts/mixed-qualified-browser-health.txt` and
`artifacts/mixed-browser-15b2da5f9014-run-0.json`.
The server lifecycle test passed in 258.656 seconds including initialization and
cleanup; 238 sampled health requests had P95 27.26 ms. Mean process CPU was
25.03% of total host capacity, not diagnostic overhead. Both browser tabs and
all owned server/sender processes were closed. Original media timing/network,
AI/clip workload and the 30-minute baseline/enabled gate remain unverified.

`Tools/Tests/check_replay_sender_pacing.py` now isolates workload transport: it
uses the full-server harness's exact loopback sender and a packet-copy-only
FFmpeg receiver, recording monotonic receipt times of FFmpeg progress records.
Timeouts kill only the owned receiver; the existing sender context guarantees
listener cleanup. Sources are bounded local files; no camera/session extraction,
Witness decode or browser playback is involved. Two unit tests check exclusion
of startup samples and rejection of insufficient/non-advancing evidence.

Independent 60-media-second audits measured media/wall slopes after the first
ten wall seconds of 0.999629 (4K25) and 1.000114 (1080p20), with zero receiver
diagnostic bytes. Reports: `artifacts/qualified-2160p25-sender-pacing.json` and
`artifacts/qualified-1080p20-sender-pacing.json`. Thus the standalone sender does
not inherently supply the approximately 4–6% slower clock seen in the mixed
Witness/browser path. This does not exclude backpressure when Witness consumes
the stream, or hardware/render pressure when the browser decodes it.

Inspection identified a confounder requiring a controlled comparison:
`CameraWorker::WorkerMain` sleeps to nominal codec frame duration for every
input not classified as `rtsp://`. Our live paced HTTP MPEG-TS fixture therefore
receives additional worker pacing; production RTSP inputs bypass it. Requested
sleep may overshoot on Windows, and a codec's declared cadence need not match a
timestamp-normalized container. This is a candidate cause, not an A/B-proven
diagnosis. Correctly distinguish live inputs from recorded-file pacing and
rerun the same matrix before using this fixture for performance acceptance or
claiming a production playback fix.

### Live-input versus recorded-input worker pacing (2026-10-05)

`InputStream::RequiresPacketPacing` now caches the opened demuxer's normal
seekability after probing. A seekable local/remote recording keeps the worker's
existing nominal-rate pacing; a non-seekable transport, or demuxer without a
generic AVIO context (such as RTSP), does not receive a second worker sleep.
Before opening and after shutdown the policy is conservatively reset to pacing.
The cached value is queried on the owning camera worker, not exposed as an
unsynchronized cross-thread snapshot. State remains private to WitnessCamera's
PIMPL; the public class layout is unchanged, but the new exported method requires
deploying the matching rebuilt server and camera DLL together.

This replaces the worker's old RTSP-URL-only special case, rather than assuming
every HTTP URL is live. Seekability is a transport capability, not a universal
semantic live/file classifier: a finite non-seekable recording cannot be
distinguished from a live transport by this policy and receives no nominal-rate
sleep. Existing file pacing itself still uses codec frame duration, not a new
DTS-based audio/video replay scheduler. Neither behavior is claimed solved here.

`InputPacingProbe` is an opt-in native test binary, excluded from deployment.
The integration test opened actual H.264 720p and HEVC 4K local files, the same
files from ephemeral loopback HTTP byte-range servers, and live paced HTTP
MPEG-TS senders. It verified pacing true/true/false respectively, as well as the
before-open and after-shutdown reset. All six media/transport cases passed in
4.639 seconds (`artifacts/live-input-pacing-range-integration.log`). No physical
cameras or production configuration/database were accessed.

Coherent package: `build-vs2026-stable/deploy/live-input-pacing-e6f74ec3f350`,
native e6f74ec3f350 / web b9437265ce28, 94 files / 177,675,740 bytes, verified
against runtime in `artifacts/live-input-pacing-package-verified.json`. Native
build completed with no C++/linker warnings and reused the immutable dependency
environment; no vcpkg install occurred. Frontend chunk-size advisories remain.

The sixteen-stream mixed matrix passed both 60-second sampled runs, actual
authenticated health/history checks and percentile resets, and graceful restart
and shutdown (150.300 seconds including initialization/cleanup). Preserved
reports: `artifacts/live-pacing-e6f74ec3f350-run-{0,1}.json` and
`artifacts/live-input-pacing-server-regression.log`. Across 960 camera snapshots
per run, none reported disconnected state, established packet drops or timestamp
rejection. End-to-end health-request P95 was 26.61 / 26.59 ms.

For representative camera IDs 1/9/13, accepted-packet slopes between first and
last sampled snapshots were respectively 16.66/20.00/25.01 packets/s in run 0
and 16.65/20.01/24.99 in run 1. The earlier identical server-only replay run on
native 15b2da5f9014 measured 16.63/19.85/23.78. This supports removal of additional
HTTP worker pacing as the fix for the replay's under-delivery; it is not a new
RTSP/Tapo-production fix. Process CPU increased to mean 28.67 / 27.53 percent
with the higher delivered workload, so this is not a diagnostic-overhead A/B
comparison. Browser rendering after the pacing fix and the representative
30-minute baseline/enabled phases still need verification; zero server drops
must not be used as a smooth-browser-playback claim. All test-owned processes
stopped. No main merge, remote push, production deployment or wake lock occurred.

### Mixed browser comparison after live-input pacing (2026-10-05)

The same native e6f74ec3f350 / web b9437265ce28 package passed the isolated
240-second browser-attached fixture (260.173 seconds including lifecycle).
Actual MSE players showed all sixteen expected native resolutions, unpaused
1x playback, readyState 4 and no media errors at three live observations.
At roughly 9/48/100 seconds, 4K buffered headroom remained about 1.1–1.4
seconds, replacing the previous build's draining headroom. The fourth DOM
observation and dashboard JPEG were taken **after the fixture stopped**;
their Connection Lost/reset media state is not live playback evidence.

The final persisted player heartbeat, at approximately 195 seconds, had all
sixteen players buffered/playing, zero restarts/stalls/errors, and empty append
queues. 4K players still dropped 180–197 of 4,807–4,861 presentation frames
(about 3.7–4.1%). This demonstrates improved delivery/headroom, not zero browser
drops or smooth playback under every load, and does not prove a hardware cause.
Persisted history had 15 durable samples, zero pending/rejected/storage failures.
Evidence: `artifacts/live-pacing-browser-e6f74ec3f350-{run-0,history}.json`,
`artifacts/live-pacing-browser-health.txt`, the read-only DOM observations in
`artifacts/live-pacing-browser-observations.json`, and
`artifacts/live-pacing-browser-regression.log`. Both temporary tabs and all
fixture processes were closed. AI/clip workload and the full overhead gate
remain separate, unproved requirements.

### Recording-enabled workload qualification (2026-10-05, in progress)

Separate branch `codex/representative-load-harness` adds opt-in
`WITNESS_TEST_ACTIVE_DETECTION=1` to the isolated lifecycle fixture. It selects
CPU ONNX with a 1 fps motion-inference cap, a 0.01 MV threshold, continuous DVR,
and explicit authenticated manual recording on every camera. Normal authentication
remains enabled. Numeric completed-inference log windows and newly created
clip/continuous-segment rows are required for every camera on each lifecycle;
previous-run rows/logs cannot qualify a restart. Rows alone do not prove decoded
recording integrity, and manual clips do not prove automatic motion activation.

The first 90-second H.264 720p / HEVC 4K qualification exposed an important gap:
both cameras completed two baseline inferences, but no clips or continuous
segments were created. Merely lowering the MV threshold did not activate motion
in these replay inputs, and the original camera seed left DVR disabled.
Evidence is preserved in `artifacts/representative-workload-initial-{no-clips,health}.json`.
This is not representative sustained motion-driven inference load.

After explicitly enabling DVR and starting/stopping normal authenticated manual
clips, the lifecycle timed out during shutdown (30-second limit; 130.944 seconds
total). `artifacts/representative-workload-recording-qualification.log` records
that failure. Test-owned processes were killed and their private files cleaned
up; no production configuration was changed. This is a separate shutdown-hang
investigation, not a passing workload qualification or proof of another double
join. A follow-up reproduction now preserves failure logs and, when the installed
Windows debugger is available, captures non-invasive stacks from only the owned
fixture server before termination.

Five pure evidence-validator tests pass, covering continuous sample/endpoints,
sleep/partial/reordered observation rejection, zero-inference/model-ready
non-evidence, numeric-only log export, and old recording-row exclusion. Observation
gaps over ten seconds invalidate a continuous test. The full 16-stream, 30-minute
baseline/enabled CPU/processing/HTTP performance gate remains outstanding. Native
product binaries are unchanged; the verified e6f74ec3f350 package is reused.

Follow-up evidence: the identical two-camera AI/recording reproduction passed
both 90-second sampled lifecycles, including restart/shutdown (227.047 seconds
total). Each lifecycle created one new manual clip and one continuous segment
per camera, and logged two completed baseline inferences per camera. Observed
maximum sampling gaps were 1.027 / 1.027 seconds; health-request P95 was
25.76 / 25.02 ms. These sparse baseline inferences must not be described as
sustained 1 fps motion inference. Preserved results are
`artifacts/recording-ai-e6f74ec3f350-{workload,health}-{0,1}.json` and
`artifacts/representative-workload-shutdown-diagnosis.log`.

`WITNESS_TEST_RECORDING=1` now separates actual recording from detection, and
`WITNESS_TEST_LIFECYCLE_RUNS` allows 1–10 bounded lifecycle repetitions (default
two; browser mode remains one). Six 10-second sampled recording-only lifecycles
passed in 98.477 seconds, each requiring fresh clip/DVR rows for both cameras.
Results are `artifacts/recording-only-e6f74ec3f350-{workload,health}-{0..5}.json`
and `artifacts/recording-shutdown-six-lifecycle.log`. Thus the original timeout
has **not** reproduced or been diagnosed; no native shutdown change was made.
It remains an unresolved observation, not a proved recording deadlock.

Optional `WITNESS_TEST_VERIFY_RECORDINGS=1` applies the existing bounded,
local-only strict decoder/timestamp inspector to newly generated outputs before
fixture cleanup. Paths must resolve inside the private cache. Two additional
10-second sampled recording lifecycles passed in 93.077 seconds including full
inspection: all eight output files decoded without diagnostics, packet/frame
counts matched, and timestamps had no missing/duplicate/backward values. This
checks the complete short files, not perception, original camera/network timing,
or a 30-minute recording. Evidence:
`artifacts/recording-integrity-e6f74ec3f350-{workload,health}-{0,1}.json` and
`artifacts/recording-output-integrity.log`. Six pure validator tests now pass,
including rejection of external DB paths before decoding and removal of private
paths from exported inspection evidence. All fixture processes stopped and
private runtime/database/cache folders were removed. No main merge, remote push,
production deployment, camera movement or wake lock occurred.

### Browser measurement-window readiness (2026-10-05)

The earlier browser fixture started its sampling clock immediately after printing
the loopback URL. Browser attachment and initialization were therefore part of
that window: a 1,800-second **server** phase did not establish a full 1,800-second
browser-playing phase. The currently running mixed-recording endurance job uses
that earlier code and will retain this limitation; editing the harness does not
retroactively alter or restart its loaded process.

Future browser phases now wait up to five minutes for an authenticated fixture
user heartbeat from the expected web build and dashboard path, received within
15 seconds, from a visible dashboard. It must contain exactly the expected MSE camera IDs, with playing
events, readyState >= 3, open sockets, actual rendered-frame counters, initialized
buffers, no initialization/keyframe wait, and no reported media errors. Synthetic
setup telemetry with an empty player list cannot satisfy it. The measured timer
starts only afterward, with refreshed server packet/frame baselines. Independent
DOM/visual observation remains necessary; telemetry alone is not proof of
perceptually smooth rendering.

Seven pure validator tests pass. A replay-clock check against the preserved real
16-player heartbeat also accepted its actual exported schema. The new live
five-minute startup/wait integration has not yet run. This gate uses the normal
authenticated history endpoint during warmup; a future baseline configuration
that disables that endpoint/history must provide an independently validated
readiness signal rather than silently removing the gate. No native builds or
other CPU-heavy tests were run during the active endurance measurement.

The ongoing endurance run has already exposed a browser-suspension/recovery
case, not uninterrupted healthy playback. A normal UI export saved as
`C:/Users/icsta/Downloads/witness-full-debug-2026-10-05T01-54-02-445Z.json`
contains watchdog delays of 34–60 seconds, paused media while fresh fragments
continued appending, and a visibility transition back to visible followed by
drift recovery. Camera 1's playhead was about 11.4 seconds with buffered media
ending at 152.5 seconds on that transition; after recovery it returned to roughly
1.2 seconds of headroom. No manual reload or playback seek was performed.
Native inputs remained connected at the captured health snapshot. The fixture's
authenticated history is preserved in
`artifacts/mixed-recording-30minute-midpoint-history.json`; DOM observations are
in `artifacts/mixed-recording-30minute-{third-media,bad-client}.json`.
This evidence does not establish a server transport failure or production
stability, and the final server measurement must not be labeled a full browser
playback acceptance pass. Visibility at startup alone cannot guarantee that a
browser stays scheduled/rendering throughout a long phase.

### Endurance result and compact browser evidence (2026-10-05)

The 16-stream recording/CPU-inference fixture finished successfully in
1,859.170 seconds, including shutdown: 1,778 server samples across the 1,800-second
phase, maximum observation gap 1.198 seconds. All 28,448 available stream
observations were connected, with zero timestamp-normalization rejection and
zero established drops/repairs, lifetime corrupt packets or missing DTS. Every
fixture camera created one manual clip and seven DVR segment rows; completed
inference windows were observed for every camera. This is sparse no-motion
baseline inference, not sustained active-scene inference. Full strict decoding
of the long recordings was not enabled. Process CPU averaged 33.69% of host
logical capacity (P95 44.98%); snapshot request P50/P95/P99 were
7.32/29.62/38.72 ms, maximum 197.76 ms. These are enabled-run observations, not
incremental instrumentation overhead without a corresponding baseline.

The history export contained 118 rows, zero pending/rejected/storage failures.
Its final hidden-browser heartbeat still reported 3–11 player restarts and high
latencies; this contradicts uninterrupted healthy browser playback. The native
lifecycle pass must not be interpreted as a client stability pass. Preserved
artifacts: `artifacts/mixed-recording-30minute-{health-0,history,workload-0}.json`
and `artifacts/mixed-recording-browser-30minute.log`. The fixture stopped and
removed its private runtime/cache/databases. No wake lock was held.

The existing compact heartbeat discarded all drift/events when full evidence
exceeded 60 KiB. It now first reduces each player to two events, one scalar drift
sample, and a scalar last-drift-restart summary. This preserves visibility,
pause state, timer delay, playhead/buffer end, and fragment/append age without
buffer arrays, messages or nested histories. Only an additional over-budget
fallback removes these details. Normal authentication, CSRF and server retention
bounds are unchanged. Offline replay of this real 16-player export retained
drift and last-drift-restart evidence for every player in 24,156 bytes (the
uncompacted report was 1,610,149 bytes). Four reporter tests, including this
optional offline export, three drift/privacy tests and four playback-guard tests
passed. The tests confirm schema/bounds, not live persisted receipt of the new
web bundle or resolution of the suspended-playback behavior.

The frontend-only `SyncWebUI` build passed type checking and bundling; no native
recompile or dependency installation was requested. Sass deprecation and large
chunk warnings remain. Verified separate package:
`build-vs2026-stable/deploy/client-health-evidence-a62e819-native-e6f74ec3f350`,
native `e6f74ec3f350` / web `32faba58be9d`, 94 files / 177,676,403 bytes.
Byte/identity verification is in
`artifacts/client-health-evidence-package-verified.json`.

A fresh normal-authenticated, loopback-only two-stream browser fixture using
that package passed in 112.065 seconds including setup, externally attached
browser readiness and clean shutdown. The new readiness gate actually accepted
both expected players before starting 60 measured seconds (60 samples, maximum
gap 1.028 seconds). Persisted real-browser records carry web identity
`32faba58be9d`, both readyState 4 / media:playing, zero restarts/stalls, and four
drift samples per player. Seven history rows persisted with zero pending,
rejected or storage failures. Anonymous health/history requests remained denied
by the fixture assertions. Native input and new web identities are intentionally
independent; the Camera DLL and server native identity still match.

Evidence: `artifacts/client-health-evidence-browser-{run-0,history}.json` and
`artifacts/client-health-evidence-browser-readiness.log`. The live screenshot
`artifacts/client-health-evidence-browser-live.jpg` was captured before fixture
shutdown. A later empty/paused DOM observation occurred after its scheduled
shutdown and is not a media regression. All test processes, private fixture
folders and agent-created tabs have been cleaned up. This short two-player test
verifies bundle startup, readiness timing and authenticated persisted telemetry;
the 16-player compaction fallback is verified offline, not yet by a new live
16-player run. Longer uninterrupted browser validation, representative active
AI load, and a diagnostics-off/equivalent baseline remain outstanding.

### Hidden-document MSE recovery candidate (2026-10-05)

The endurance export showed browser pause/throttling while WebSocket fragments
continued appending. The MSE watchdog nevertheless attempted playback resume,
frozen/stuck recovery and drift restarts while the document was hidden. That
cannot repair a browser-suspended decoder, and replacing its pipeline can abort
pending play promises. The candidate defers only these playback-specific
actions while hidden; connection/startup/append timeout checks above this guard
remain active. It clears playback timers on hidden/resumed transitions, logs
both transitions, and continues receiving/processing media.

Because a hidden playhead can stop indefinitely, ordinary playhead-based
back-buffer removal alone is insufficient. Hidden removal now follows the
buffered live edge, retaining a 30-second time window (five seconds during
aggressive quota recovery). Visible back-buffer behavior remains five seconds,
or one second aggressively. This is a time-window policy, not a strict byte cap
or a promise that the browser keeps decoding while hidden. The existing visible
keyframe catch-up/drift recovery still applies on return; without safe keyframe
evidence a large accumulated gap can require a pipeline restart. Cameras merely
hidden by dashboard layout are not affected while their document stays visible.

Three pure visibility/eviction tests pass, as do four playback-guard, three
drift/privacy and three normal health-reporting tests (the optional offline
fixture test was not selected in this invocation). Actual MSE hide/resume,
buffer eviction and persisted transition evidence still require live validation.

The initial candidate's verified package is
`build-vs2026-stable/deploy/mse-hidden-recovery-486819b-native-e6f74ec3f350`,
native `e6f74ec3f350` / web `c469dae5db3d`, 94 files / 177,677,077 bytes.
Its three-minute two-HEVC-stream normal-authenticated fixture passed in
235.596 seconds: 179 server samples, maximum gap 1.028 seconds, zero player
restarts/stalls and zero history pending/rejected/storage failures. Thirteen
player-bearing heartbeats were visible and only the final one hidden. This
does not verify a long hidden window, eviction, or return to visible. The
in-app browser's presentation-hide control was already false while its document
still reported visible; opening the health tab did not reliably induce the
desired hidden state. Evidence is preserved in
`artifacts/mse-hidden-recovery-browser-{run-0,history}.json` and
`artifacts/mse-hidden-recovery-browser.log`.

`web-ui/tests/mseHiddenRecovery.test.mjs` now drives the **production MSE
composable**, using in-memory socket/SourceBuffer/video/timer primitives and a
virtual clock. A 90-second hidden interval delivers 360 more partials while the
playhead is frozen: no restart or resume attempts, an open socket, removal keeping
the retained time range within 30 seconds, and playback-specific recovery restored
on return to visible. A separate case proves stalled appends still recover while
hidden. These primitives do not model actual browser demux/decoder behavior or
real SourceBuffer keyframe-removal semantics.

The tests also exposed an independent existing latch-off path: an already-open
socket could stop delivering fragments while paused/ready media never met the
stuck/frozen conditions. The watchdog now treats ten seconds without fragments
after initial buffering as `fragmentTimeout`, regardless of document visibility,
before applying the playback suspension guard. This condition requires prior
fragment progress, an initialized buffer, an open socket and no pending reconnect;
existing scheduling-delay compensation still applies. A third production-
composable test verifies hidden delivery-stall recovery. All three pass. Running
the same cases against read-only pre-fix source `a621c7c` fails all three as the
negative control (`artifacts/mse-hidden-recovery-baseline-regression.log`).
The later fragment-timeout change is not present in the initial candidate
package above and still requires a newly built package/live validation.

The final fragment-timeout candidate built/type-checked successfully and was
packaged separately as
`build-vs2026-stable/deploy/mse-hidden-delivery-2fd84da-native-e6f74ec3f350`:
native `e6f74ec3f350` / web `001cf059def2`, 94 files / 177,677,267 bytes.
`artifacts/mse-hidden-delivery-package-verified.json` confirms byte/identity
coherence against stable build output. No dependency rebuild, native recompile,
main merge, remote push or deployment occurred. Existing Sass/chunk warnings
remain. The new in-memory watchdog and visibility tests now resolve their source
relative to the test file and pass from the repository root as well.

A fresh authenticated three-input browser fixture (accepted Tapo H.264 720p
replay, qualified HEVC 1080p20 and HEVC 4K25) passed in 127.463 seconds including
setup/readiness/shutdown. Its 60 measured seconds contained 60 native samples,
maximum gap 1.027 seconds; all inputs stayed connected. The final persisted
player heartbeat had readyState 4 and zero restarts/stalls/media errors for all
three, headroom 1.19/1.28/1.21 seconds, and frames/dropped counters
1212/2, 1474/0 and 1845/87 respectively. The roughly 4.7% 4K frame drop fraction
is an observed rendering limitation, not loss-free playback. Browser history
pending/rejected/storage failures were zero. Evidence:
`artifacts/mse-hidden-delivery-browser-{run-0,history}.json`,
`artifacts/mse-hidden-delivery-browser.log`, and the live pre-shutdown screenshot
`artifacts/mse-hidden-delivery-mixed-live.jpg`. Fixture-owned processes/data and
all agent-created browser tabs were cleaned up; no wake lock was held.

This confirms normal mixed-codec startup/persisted health with the final bundle.
The hidden-frozen/eviction/resume and actual silent-delivery paths are covered by
the real-composable **mocked primitive** regression tests; prolonged actual
browser suspension/SourceBuffer behavior still needs qualification. Neither this
short replay nor the earlier 30-minute native run establishes production
stability or incremental diagnostics overhead against an equivalent baseline.

### Actual hidden/resume qualification and recovery-history follow-up

The final candidate's two-HEVC browser qualification completed successfully in
431.742 seconds (360-second measured browser phase). Normal authenticated history
captured an 80-second hidden interval with frozen/paused playheads, fresh fragment
and append progress, and no hidden playback restarts/stalls. After a longer untouched
interval, actual DOM buffer ranges retained 29.25/29.76 seconds while the old
playheads were outside those ranges. Foreground recovery required no reload or
manual seek: both players returned to readyState 4, normal speed and roughly
1.3 seconds of live headroom. Each recorded one visible drift restart, zero stalls.
Evidence is preserved in `artifacts/mse-hidden-real-browser-{run-0,history}.json`,
`artifacts/mse-hidden-real-browser-before-resume.json`,
`artifacts/mse-hidden-real-browser-{resume,recovered}-dom.json` and
`artifacts/mse-hidden-real-browser-recovered.jpg`. This validates actual hidden
buffer eviction/resume for these two HEVC replays, not production or 16-stream
overhead qualification. Fixture processes/data were cleaned up; no wake lock.

This run exposed a separate backend omission: `lastDriftRestart` survived client
compaction but was discarded by the server allowlist. The follow-up keeps its
bounded timestamp/type/reason/lag/generation and scalar causal drift snapshot.
Free-form messages, arbitrary nested data and buffer arrays remain excluded;
ownership/authentication and the payload/history bounds are unchanged. Native
allowlist regression tests pass. The normal-authenticated full-server fixture also
passed in 14.282 seconds across two starts, verifying the same summary through
POST, asynchronous durable storage, GET and server restart. Anonymous health and
history access remained forbidden. The first invocation failed before starting
the server because its inherited stdin handle was invalid; the successful rerun
used an explicit DEVNULL stdin. No production database or session was used.

The updated server/frontend build succeeded with existing Sass/chunk advisories.
The separately packaged candidate is
`build-vs2026-stable/deploy/client-health-recovery-2d206fa-native-369a82b441e1`,
native `369a82b441e1` / web `5686d78a4693`, 91 files / 122,140,627 bytes (without
optional PDBs). `artifacts/client-health-recovery-package-verified.json` confirms
inventory/identity/byte coherence. Dependencies were reused. This package has not
yet received a new long mixed-stream browser run, production deployment or the
equivalent native diagnostics-overhead A/B acceptance test.

The next 16-stream qualification exposed a readiness-validator false negative:
healthy Tapo playback can leave `media:ratechange` as its latest event rather than
`media:playing`. The corrected predicate requires positive measured playback
advance over a bounded sample interval and an explicitly visible, unpaused drift
sample, in addition to the existing authenticated identity, fresh report, matching
build, exact player set, ready/open/initialized and zero-error requirements.
Explicit waiting/stalled/paused/ended/error events still fail. `seeking`, which
compact reports omit, is rejected when explicitly true; absence alone does not
replace the required progress measurement. Seven validator tests pass, including
rate changes, frozen playheads, hidden/paused samples, missing samples and NaN.
The saved real 16-player heartbeat in
`artifacts/client-health-recovery-ratechange-readiness.json` passes the corrected
predicate. The already-running process loaded the old predicate: none of its
pre-gate time counts toward endurance, and it must finish before a new attempt.

That old-predicate attempt terminated with the readiness timeout in 324.060
seconds and cleaned up. A fresh corrected-predicate attempt then passed the
readiness gate (`browserPlaybackReady=true`) with all 16 actual DOM videos
visible, unpaused, readyState 4 and no media errors. Its bounded 1,800-second
phase is still running at this checkpoint; no endurance result is claimed.
Log: `artifacts/client-health-recovery-progress-gate-16stream-30minute.log`;
startup screenshot: `artifacts/client-health-recovery-16stream-start.jpg`.
The verified deployment remains unchanged by this test-harness correction.

During that corrected run, all 16 playheads were automatically paused when the
in-app browser became hidden, while incoming media and heartbeats continued with
zero restarts/stalls. Explicitly foregrounding the browser recovered every player
with one drift restart. Later DOM observations showed all 16 visible, unpaused,
readyState 4, normal speed, no media errors and 1.12–1.41 seconds of live headroom.
The new native allowlist retained each actual `lastDriftRestart.drift` snapshot in
authenticated history, not just the synthetic fixture event. Evidence:
`artifacts/client-health-recovery-16stream-after-foreground-history.json`,
`artifacts/client-health-recovery-16stream-recovered-dom.json` and
`artifacts/client-health-recovery-16stream-recovered.jpg`. Endurance is still
running; the initial hidden period is not continuous visible playback evidence.

### Functional packet history separation prerequisite

The timestamp duplicate/regression guard previously queried the diagnostic packet
ring. Disabling that ring for an overhead baseline would silently change repair
decisions. On the separate `codex/functional-packet-history` branch, a fixed-size,
Camera-DLL-private `RecentWrittenVideoPackets` retains only the hashes and relative
disposition sequence needed by correctness. Every recorded disposition ages this
history, including audio and rejected packets. Lookup still checks at most the
last 32 written video packets within the last 1,024 dispositions; it does not
extend the duplicate horizon or reset it at reconnect. Existing trace collection
remains enabled and unchanged. This is a prerequisite, not a diagnostics-off
switch or a completed overhead measurement.

The standalone native regression passes boundary tests and 80,000 lookup
comparisons over 20,000 deterministic mixed/sparse-video dispositions against an
independent reference implementation of the original ring lookup. It uses no
camera, network or production data. Adding its test target reconfigured CMake
(8.1 seconds) and generated projects (2.0 seconds) using the same immutable
managed dependency environment; no package install/rebuild occurred. That brief
test compilation overlaps the current endurance phase, so its CPU measurements
must not be represented as a clean overhead baseline. Integrated Camera/server
build, timestamp/media regression fixtures and a separate deployable package for
this source change remain pending after the frozen-package endurance run.

`LiveDuplicateHistoryTests` adds an offline production-mux check: feed 40
monotonic video packets, replay the same encoded payload with a bounded backward
timestamp, and require duplicate rejection both with and without the optional
trace retained. A third run deliberately clears functional evidence and requires
acceptance/repair, so the check cannot pass merely because equal DTS is always
rejected. Its object-only MSVC compile passed with zero warnings/errors in 5.99
seconds; no Camera/server binaries were compiled or linked. Execution against a
matching rebuilt Camera DLL remains pending. CMake reused the same managed
dependencies (7.0-second configure / 1.3-second generation).

The running 16-stream package later reported one error count per player after
the foreground restart, despite continuing presentation, no additional restarts,
and roughly 1.1–1.4 seconds headroom. The bounded persisted recent-event lists
had already evicted the error events. Do not infer an AbortError, WebSocket close
or decoder failure from that counter alone. This exposes another history
requirement: retain a bounded structured last-error snapshot independently of
the rolling routine-event list, without persisting free-form messages or secrets.

On the separate `codex/client-health-error-evidence` branch, MSE diagnostics now
retain one last counted pipeline/transport error independently of routine events.
The report and server allowlist preserve its bounded time/type/name/reason,
numeric stage/generation/socket context, deliberate-close flag and scalar drift
snapshot. Free-form messages, arbitrary nested payloads and drift/buffer arrays
are not persisted. Error counters and recovery behaviour are unchanged; a
transport error during deliberate teardown is retained and distinguishable rather
than silently reclassified. The attached drift sample is the most recent sampled
state, with its own timestamp, not a claimed simultaneous measurement.

Five production-composable mocked-primitive tests pass, including 600 routine
events evicting the counted error without losing the sticky snapshot, replacement
by a newer SourceBuffer error, and socket errors during deliberate teardown.
Three normal reporting tests pass (the optional offline export case was skipped),
including rich 16-player compaction with error evidence and no free-form messages.
Type checking passes after replacing unsupported `Array.at` with indexed access.
Native allowlist/privacy tests also pass for structured last-error context.
The full normal-authenticated storage/restart fixture has been extended, but needs
the updated native build before execution. None of this new instrumentation is in
the frozen package currently undergoing endurance, so it cannot retroactively
identify the already-evicted errors from that run. Matching native/frontend build,
runtime tests, UI verification and a separate package remain pending.

The Health table now has a readable last-error clue (append, buffer/setup,
socket or media failure; deliberate-close context when known), with the event
timestamp available on hover. Type checking passes; rendered verification still
requires the new bundle. The running old-package test cannot show this UI yet.

The corrected 16-stream run completed its measurement but failed graceful
shutdown after 30 seconds (fixture total 1,916.383 seconds). Its debugger attach
was denied with NTSTATUS C0000022; there are no usable captured thread stacks,
so do not claim a diagnosed deadlock. Evidence is preserved in
`artifacts/client-health-recovery-30minute-{run-0,history}.json`,
`artifacts/client-health-recovery-30minute-shutdown.log` and
`artifacts/client-health-recovery-30minute-hang-stacks.log` (the latter records
only the failed capture). Fixture-owned processes/data were cleaned up. Native
playback samples must be audited separately from this failed lifecycle gate.

### Owned-process shutdown dump capture (2026-10-05)

The isolation fixture now captures a normal/thread-info minidump using its
retained `subprocess.Popen` CreateProcess handle before terminating a hung child.
It never opens an arbitrary PID or captures a production process. Dump filenames
are exclusive and random; no full-process-memory flag is requested. Offline CDB
stack extraction replaces the denied live attachment. Matching PDBs still need
to be supplied for useful native symbols; a successful dump is evidence capture,
not a diagnosis. Capture failure must not mask the original lifecycle failure or
prevent owned-process cleanup.

`python Tools/Tests/test_owned_process_dump.py` passed on Windows: a newly spawned
Python child produced an `MDMP` file, overwrite was refused, the child stayed live
after capture, and an exited child was rejected. This required elevated execution
because the runner could not access its own newly created temporary directory.
Reproduction of the server hang and inspection of its new dump remain pending.

### Current matched candidate and loaded lifecycle checks (2026-10-05)

Native `263bb28c9d9c` and web `e86cf29a7b1e` rebuilt successfully using the
existing pinned VS2026 dependency cache (manifest install/bootstrap still off).
Sass deprecation and large-chunk warnings remain. The separate package
`build-vs2026-stable/deploy/client-error-evidence-native-263bb28c9d9c` includes
matching symbols and verifies at 94 files / 177,697,489 bytes; verifier output is
`artifacts/client-error-evidence-native-263bb28c9d9c-package-verified.json`.
Older native `369a82b441e1` symbols are preserved separately under
`artifacts/native-369a82b441e1-shutdown-symbols`.

The rebuilt production duplicate-history regression passed both H.264 and HEVC,
including the negative control and clearing only optional traces. Cadence/pause
regressions passed for both codecs with newly generated 150-second 20fps no-B
synthetic fixtures. The initial H.264 cadence attempt used a 29-second replay
window and was not a valid pass; the subsequent adequately long fixtures are the
evidence. All eight cadence outputs strictly decode with no missing, duplicate or
regressing packet/frame timestamps (`artifacts/native-263bb28c9d9c-cadence-decode.json`).
Paced timestamp-rejection recovery and eight cross-DLL ownership/shutdown cycles
also passed for each codec. All four saved recovery generations pass the same
strict inspection (`artifacts/native-263bb28c9d9c-recovery-decode.json`). These small
synthetic tests do not cover network jitter, every audio codec, B-frames or
perceptual smoothness on physical cameras.

The real normal-authenticated endpoint/storage/restart fixture passed in 14.943
seconds, including the new structured last-error allowlist and anonymous denial.
Two 90-second 16-stream recording/CPU-detection lifecycles then passed in 214.048
seconds total with graceful exits; each has 16 recording rows, 16 continuous
segment rows and 16 completed-inference windows. Evidence is preserved in
`artifacts/owned-shutdown-263bb28c9d9c-{16stream-run,workload}-{0,1}.json` and its
90-second test log. Strict recorded-output inspection was not enabled; rows prove
creation, not playback quality. Four paced input recordings repeated four times
are not 16 independent cameras. Inference includes the sparse no-motion baseline
and has a 1fps ceiling; this is not proof of sustained active 1fps inference.
No browser was attached during these two short cycles, so they do not resolve the
previous longer browser-connected shutdown failure.

A new 30-minute, 16-stream browser-connected run has passed its authenticated
positive-playback readiness gate using this immutable package, with all 16 actual
videos visible, readyState 4, unpaused and without media errors at initial
inspection. It is still running; completion/shutdown and continuous visibility
remain unproven. Startup proof: `artifacts/client-error-evidence-16stream-start.jpg`;
live test log: `artifacts/client-error-evidence-263bb28c9d9c-16stream-30minute.log`.
No compilations are planned during its measured phase. This still is not the
required diagnostics-off/on comparison; the full overhead gate remains open.

### Completed visible endurance and queue-stop regression (2026-10-05)

The native `263bb28c9d9c` / web `e86cf29a7b1e` browser-connected test passed,
including graceful shutdown and owned fixture cleanup (1,919.318 seconds total).
Its 1,800-second measurement has 1,778 samples, maximum observation gap 1.165
seconds. All 16 cameras were connected at the end. There are 120 real-player
history rows covering 1,788.763 seconds; every retained session reports visible,
and final player counters total zero restarts, stalls or errors. Periodic DOM
checks also found all 16 videos playing. The heartbeat span is not an exact
1,800-second continuous compositor trace. Storage failures are zero.

Health-request wall time P50/P95/P99/max was 7.90/30.03/43.21/165.09 ms. Final
rolling HTTP handler P95 was 4.528 ms and event-loop occupancy P95 5.575 ms.
Host CPU P50/P95 was 66.28/82.81%; process CPU normalized to host capacity was
34.47/44.23%. These are enabled-phase measurements, not overhead deltas. Final
per-camera rolling processing P95 ranged 2.72–15.55 ms, excluding queue wait and
decode. Browser presentation drops remained: about 0.2% on the first H.264
fixture, 1.7% on the second, 0.01–0.02% on 1080p HEVC and 4.7–4.8% on 4K HEVC.
Zero error/restart counters do not prove flawless presentation or identify the
cause of those drops. No compilation ran during measurement.

Final evidence: `artifacts/client-error-evidence-30minute-final-{run-0,history,workload}.json`
and compact audit `artifacts/client-error-evidence-30minute-audit.json`. The run
does not reproduce or establish the cause of the earlier shutdown hang. Four
paced recordings repeated four times do not replace independent physical camera
or broader codec/audio/protocol validation. The genuine diagnostics-off/on load
comparison and its CPU/processing-P95 thresholds remain unproven.

Independent source inspection found that image queue worker entry reset its
shared stop flag and shutdown wrote that flag outside the waiter's mutex. The
queue-stop change removes that reset, explicitly initializes the flag and updates
it under QueueMutex before notifying all waiters. It is a permanent stop for the
queue instance, not a change to frame scheduling, queue priorities or public DLL
layout. No attribution to the previous server hang is claimed.

The public-API `ImageQueueShutdownTests` executable covers a fresh waiting worker,
late entry after repeated stop requests, and eight concurrent waiters. The same
executable was run with the frozen legacy DLL in a private temporary runtime and
the fixed DLL: it verifies each actually loaded native identity, requires the
legacy negative control to lose the late-entry stop request, and requires the
fixed DLL to pass. This check passed in 10.134 seconds. Legacy failure is safely
unblocked using a sentinel; child processes also have 15-second timeouts.

The fixed native `58d250ffc32a` / web `6c558bdd9d9e` rebuilt without native compiler
warnings or dependency installation (Sass/chunk warnings remain). Two 90-second
16-stream recording/CPU-detection lifecycles passed with clean shutdown in
214.511 seconds total; both produced 16 clip rows, 16 continuous segment rows and
16 completed-inference windows. Saved evidence:
`artifacts/processing-queue-58d250ffc32a-{replay,workload}-{0,1}.json` and
`artifacts/processing-queue-shutdown-baseline-fixed.log`. Strict recorded-output
inspection was not enabled; these short, browserless runs are not the full load
gate. The separate `build-vs2026-stable/deploy/processing-queue-shutdown-native-58d250ffc32a`
package verifies at 94 files / 177,697,489 bytes with matching symbols; output:
`artifacts/processing-queue-58d250ffc32a-package-verified.json`. Longer validation
of this queue-fixed native build and rendered last-error UI verification remain
pending.

### Rendered persisted-error evidence and optional metadata (2026-10-05)

The queue-fixed native `58d250ffc32a` / web `6c558bdd9d9e` completed the
four-minute, three-stream, authenticated browser fixture with graceful shutdown
(296.956 seconds total). Its 240-second measurement has 238 samples and a
maximum observation gap of 1.029 seconds. All 18 retained real-player reports
were visible; final players had readyState 4, zero restarts/stalls/errors and
latencies 1,228/1,371/1,126 ms. Presentation drops were 11/4,139, 3/5,063 and
308/6,331 frames respectively. These are paced H.264/1080p HEVC/4K HEVC
recordings, not physical-camera or diagnostics-overhead qualification.
Evidence is preserved in
`artifacts/client-health-rendered-58d250ffc32a-final-{history,replay}.json` and
`artifacts/client-health-rendered-58d250ffc32a-3stream.log`.

Rendered inspection found that the fixture's minimal stored report displayed
`Invalid Date` and an empty build identity. The health view now formats missing
or invalid metadata as `Unknown`, retaining the otherwise useful report rather
than rejecting it. Valid sample dates still use the browser's local formatting.
Three new tests cover malformed/missing values, valid local formatting and the
table's formatter wiring. The full client suite passed 58 tests, with one
optional external-export case skipped (`artifacts/client-health-display-ui-regressions.log`);
type-check and Vite production build also passed. Third-party Sass and chunk
warnings remain.

The standalone monitor route was inspected using a normal authenticated
disposable fixture. Both the live players and persisted synthetic error row
were rendered: `Build Unknown · sampled Unknown` and `Last error: socket error
during deliberate close`. Proof: `artifacts/client-health-metadata-detail.jpg`.
This proves display/retention of seeded error evidence, not reproduction of a
real transport error. Popup behavior was not proved by direct route navigation.

Package `build-vs2026-stable/deploy/client-health-display-native-58d250ffc32a`
contains unchanged native `58d250ffc32a`, matching symbols and rebuilt web
`b799900b1578`. Verification against a separate assembled reference passed at
94 files / 177,697,696 bytes; report:
`artifacts/client-health-display-native-58d250ffc32a-package-verified.json`.
Native files came from the previously verified queue-fixed package; no compiler
or dependency changes were made for this UI patch.

The in-app browser visibility control hid its UI but the replay page continued
to report `document.visibilityState = visible`. This action is not evidence of
hidden-tab recovery. The production-composable mocked 90-second hidden test
passes, but a genuine hidden/resume browser test and the full matched
diagnostics-off/on load comparison remain open. No production deployment,
main merge or push occurred; the user-owned backlog edits remain untouched.

The rebuilt-web fixture subsequently completed its 240-second phase and clean
shutdown in 296.390 seconds total. It has 238 native samples (maximum gap 1.038
seconds), 18 real-player history samples and zero storage failures. Final
readyState is 4 for all three players, with zero restart/stall/error counters
and playback lags 1,203/1,305/1,218 ms. Presentation drops are 10/4,152,
4/5,057 and 325/6,324 frames; particularly the 4K result is not a claim of
perfect rendering. All retained document visibility states are `visible`.
Final evidence: `artifacts/client-health-display-b799900b1578-final-{history,replay}.json`,
compact `artifacts/client-health-display-b799900b1578-audit.json` and
`artifacts/client-health-hide-resume-58d250ffc32a.log` (the log name records test
intent, not a passed hide/resume assertion). Detection and recording were off;
client regressions ran concurrently, so this is not an overhead benchmark.
Fixture processes/data were cleaned up and both owned tabs closed; browser
visibility was restored to background mode. No wake lock was taken.

### Optional packet-observer baseline preparation (2026-10-05)

Branch `codex/packet-trace-baseline` adds `WITNESS_PACKET_TRACE=0`, read once per
new live-stream instance. Absence, `1`, or any invalid value keeps the existing
default-on behavior. This is not a general diagnostics-off mode and must not
be labelled as satisfying the full overhead gate. Only filling/retaining the
1,024-packet observer ring and its derived packet-detail events are skipped.
Cheap disposition counters remain available in both phases. Summary and detailed
administrator exports expose `packetTraceEnabled` rather than representing an
empty disabled trace as complete evidence.

Payload hashes and independent recent-written history, sequence/activity IDs,
arrival/cadence clocks, source structure inspection, decode-corruption signals,
timestamp repair/requalification, fMP4 validation and transport integrity remain
active. Explicit authenticated packet capture still constructs and records its
decision metadata even with passive packet retention off; capture authorization
is unchanged. Actual capture under this mode has not yet been exercised.

The extended production-mux test writes 40 input packets and compares exact
emitted init/fragment/segment bytes with packet history retained, cleared, or
disabled before ingest. Every mode rejects the same bounded-DTS repeated payload;
clearing functional history instead must accept/repair it as a negative control.
H.264 and HEVC pass with the environment absent, `0`, `1`, and `invalid` (eight
fixture invocations; each includes all four scenarios). The disabled mode must
retain no detailed packets, report its disabled coverage, and preserve accepted,
dropped and repaired counts. Log: `artifacts/packet-trace-on-off-media-regressions.log`.
These are short video-only offline fixtures, not network, audio, broader cadence
or performance qualification.

Matched native `30fb4d0dc7ce` / web `f76c1d4fa36f` built successfully against the
same pinned dependencies, with manifest installation/bootstrap disabled. No
native compiler/linker warnings appeared; existing Sass/chunk warnings remain.
The exported streaming snapshot adds a coverage flag, so consumers must use
matching rebuilt binaries. The new private live-stream field also means internal
state-access regression executables must be rebuilt against this Camera DLL.
Separate package `build-vs2026-stable/deploy/packet-trace-baseline-native-30fb4d0dc7ce`
verifies at 94 files / 177,730,464 bytes including matching symbols; output:
`artifacts/packet-trace-baseline-native-30fb4d0dc7ce-package-verified.json`.

HTTP/SQLite timing traces, fragment/segment/media-event observers, FFmpeg log
retention, host sampling, browser event/reporting/storage work and processing
histograms are still enabled. Their functional dependencies must be mapped before
the complete matched off/on comparison. Turning off only this packet observer
cannot support a claim about total diagnostics overhead. No production
configuration, main merge, remote push or user-owned backlog edits were made.

Normal-authenticated full-server tests passed for both environment modes:
38.228 seconds with packet tracing off and 38.304 seconds with tracing on. Each
mode runs two fresh-process lifecycle phases sharing its private test database,
with three paced prerecorded sources and 10 seconds of sampled ingest per
phase. The fixture asserts the actual summary coverage flag, continued accepted
packet/processing progress, normal authentication/history durability and clean
shutdown. Detection/recording/browser playback were not enabled. Evidence:
`artifacts/packet-trace-mode-{0,1}-full-server.log` and
`artifacts/packet-trace-mode-{0,1}-full-server-run-{0,1}.json`.
No claim about long-run stability, full diagnostics overhead, or representative
16-camera load follows from these short lifecycle tests.

### Optional HTTP/SQLite observer baseline preparation (2026-10-05)

Branch `codex/server-trace-baseline` adds `WITNESS_HTTP_TRACE=0` and
`WITNESS_SQLITE_TRACE=0`, cached once per process. Missing, empty, `1` and invalid
values preserve default-on behavior. HTTP baseline mode avoids request-detail
allocation, observer mutexes and observer-only event-loop posts. SQLite baseline
mode avoids query timing clocks and retention, but still acquires/releases the
actual prepared-query mutex and executes the same statements and callbacks.
Authentication, response handling, database semantics and functional stream
checks are unchanged. Restart is required to change either setting.

Authenticated health exports explicitly identify `tracingEnabled: false` and
`unavailableReason: disabledByConfiguration`, with null unavailable timings/counts
and empty trace lists. The web health panel labels disabled timing instead of
reporting artificial zeros. The actual prepared-query wrapper and HTTP observer
pass all six environment combinations (0/0, 0/1, 1/0, 1/1, absent/absent and
invalid/invalid), including immutable settings, concurrent HTTP accounting,
query binding/read/write, callback exception propagation and mutex release.
Evidence: `artifacts/server-trace-observer-regressions.log`.

Native `23c905449993` / web `520d85d0b3da` built against unchanged pinned
dependencies with bootstrap/manifest installation off. No native compiler or
linker warnings appeared; Sass/chunk warnings remain. Matching rebuilt consumers
are required for the internal snapshot/query-instance field additions. Package
`build-vs2026-stable/deploy/server-trace-baseline-native-23c905449993` verifies
at 94 files / 177,757,012 bytes including matching symbols; verifier output:
`artifacts/server-trace-baseline-native-23c905449993-package-verified.json`.
The client suite passed 58 tests with one optional export test skipped.

Normal-authenticated full-server recording tests passed with HTTP, SQLite and
packet observers all off (117.380 seconds) and all on in a repeat (114.197
seconds). Each mode ran two lifecycle phases, three prerecorded sources and
10-second sampled windows per phase, with manual clips and continuous DVR.
All 24 output inspections passed strict decoding with zero missing, duplicate or
regressing packet timestamps. Evidence:
`artifacts/server-trace-mode-0-{replay,workload}-{0,1}.json` and
`artifacts/server-trace-mode-1-repeat-{replay,workload}-{0,1}.json`.

The first tracing-on run failed ffprobe inspection after 87.644 seconds. Its
recording/stderr were not preserved by the older fixture, so the cause remains
unknown; the passing repeat does not diagnose or fix that failure. Log:
`artifacts/server-trace-mode-1-full-server.log`. Future inspection exceptions
preserve only the already path-validated fixture recording (at most 512 MiB)
and bounded probe evidence before cleanup, while still failing the test. Eight
workload-validator and four inspector tests passed, including injected probe
failure preservation/rethrow and external-path rejection. This preservation
path has not yet captured a real server recording failure.

A separate authenticated browser fixture completed in 357.688 seconds overall,
including waiting for real playback readiness followed by a 60-second sampled
window (60 samples, maximum gap 1.027 seconds). All three players ended readyState
4 with zero restart/stall/error counters and lag 1,247/1,211/1,157 ms. Presentation
drops were 4/1,201, 0/1,465 and 94/1,849, so especially 4K is not flawless rendering.
Seven real-player history rows were retained with zero storage failures. The
rendered disabled HTTP/SQLite labels were verified, with no health-page console
errors and advancing video times on the concurrent dashboard. Evidence:
`artifacts/server-trace-disabled-browser-{history,replay}.json`,
`artifacts/server-trace-disabled-browser.log` and
`artifacts/server-trace-disabled-health.jpg`. Both owned tabs were closed and
browser visibility restored to background mode; no wake lock was taken.

These switches still do not provide a complete instrumentation-off baseline:
fragment/segment/media-event observers, FFmpeg retention, host sampling,
browser reporting/storage and processing histograms remain to be mapped. Full
matched 30-minute, 16-representative-stream overhead and genuine hidden/resume
validation remain open. No production configuration, main merge or remote push
was performed, and user-owned backlog changes were not edited or staged.

### Optional mux-history baseline preparation (2026-10-05)

Branch `codex/mux-trace-baseline` adds `WITNESS_MUX_TRACE=0`, read once when a
live-stream instance is created. Missing/invalid values retain default-on
behavior. This stops retaining the 30-segment and 180-fragment observer rings.
Summary exports expose `muxTraceEnabled`; disabled histories must not be
interpreted as complete evidence. Segment totals, durations and maximum drift
remain available. The last completed segment's normalization state is now
independent of the optional ring, preserving the same summary semantics with
history disabled. Passive anomaly snapshots are skipped when both mux and packet
history are disabled; authenticated explicit packet capture is separate.

Fragment/segment hashes and structure analysis, init checks, publication order,
transport integrity, activity IDs, functional recent-payload history and cadence
clocks remain active. The switch disables retention, not all construction or
validation work: full-off overhead qualification remains pending. Media-event
and FFmpeg retention, host/browser/history observations and processing histogram
costs still require functional-dependency review.

The production-mux regression now ingests 240 packets across actual fixture
keyframes rather than 40 packets that did not complete a segment. It requires
nonempty enabled segment/fragment history and empty disabled history, retained
summary/structure checks and identical init/fragment bytes plus event metadata,
durations, publication order and transport hashes. H.264 and HEVC pass six
environment combinations (absent, both off, both on, invalid, and both mixed
settings), with five observer scenarios plus the functional-history negative
control per invocation. Twelve invocations passed; evidence:
`artifacts/mux-trace-on-off-media-regressions.log`. The initial 40-packet assertion
failed because no segment was completed; that fixture coverage gap was corrected.

The virtual-time cadence/pause regression passes both fully retained and disabled
packet/mux histories with the required long generated 20fps fixture, including
normalization-active summaries, bounded rate learning, stable source-clock
preservation, clean-keyframe pause resets and intentional unstable-rate rejection.
All eight written control/pause/bounded-rate generations pass strict decode and
packet/frame timestamp inspection. Each of the four on/off media pairs has an
identical SHA-256 digest. Evidence:
`artifacts/mux-trace-cadence-generated-{off,on}.log` and
`artifacts/mux-trace-cadence-output-inspections.json`. An earlier attempt supplied
the 29-second Tapo sample to the >=140-second cadence test and failed its
too-short-fixture requirement; it is not a passed cadence result.

Native `02e07138f108` / web `004541adaa38` built with dependency bootstrap and
manifest installation off, against the unchanged pinned dependency environment.
No native compiler/linker warnings appeared; existing Sass/chunk warnings remain.
Matching rebuilt consumers and internal state-access tests are required for
the added snapshot/private fields. The separate package
`build-vs2026-stable/deploy/mux-trace-baseline-native-02e07138f108` verifies
at 94 files / 177,773,396 bytes including matching symbols; output:
`artifacts/mux-trace-baseline-native-02e07138f108-package-verified.json`.

Normal-authenticated full-server recording validation with mux, packet, HTTP
and SQLite observers disabled passed in 114.158 seconds: two lifecycle phases,
three prerecorded sources, 10 sampled seconds per phase, manual clips and DVR,
continued processing/packet progress and clean shutdown. All 12 outputs passed
strict decode/timestamp inspection. Actual health coverage flags were asserted.
Evidence: `artifacts/mux-trace-disabled-full-server.log` and
`artifacts/mux-trace-disabled-{replay,workload}-{0,1}.json`. Detection and browser
playback were off; these are not the 30-minute representative 16-stream gate,
network reproduction or full diagnostics-overhead qualification. No main merge,
remote push, production change or user-owned backlog edit/staging occurred.

### Optional media-event retention and independent log limiting (2026-10-05)

Branch `codex/media-event-trace-baseline` adds `WITNESS_MEDIA_TRACE=0`, read
once per live-stream instance, with missing/invalid values retaining default-on
history. Summary and detailed administrator exports expose `mediaTraceEnabled`.
Disabled mode retains no passive FFmpeg, decode hold/recovery or packet-derived
media events. FFmpeg error counters and scope `HasError`, activity IDs, packet
clocks/hashes/structure checks and player hold/recover controls remain active.

Raw-log repeat limiting is now a separate Camera-owned bounded 128-cluster
structure rather than a side effect of the optional media history. It retains
bounded normalized signatures and context, not raw messages or packet payloads.
Severity/phase/component/generation, 60-second inactivity and five-minute total
age limits remain; clearing optional event history no longer resets raw-log
suppression. Default-on logging may therefore suppress repeated errors longer
when unrelated media events previously evicted a still-active FFmpeg cluster.
That is an intentional log-flood protection change, not a corruption-handling
change. The limiter's functional clocks/context remain in the off baseline.

Actual FFmpeg callback and production-mux tests pass H.264 and HEVC with the
environment absent, off, on and invalid (eight invocations). Each compares
retained and disabled history: identical media bytes, metadata, durations,
transport hashes and hold/recovery ordering; the recovery control must precede
the exact independent fragment. FFmpeg error/warning counts and `HasError` stay
active, corruption/recovery counters agree, and enabled history retains the
first/latest numeric error variants with their activity correlation. Tests also
cover repeat-count independence after history clearing, clock/generation/context
isolation, expiry boundaries and cluster-capacity eviction. Two additional
invocations pass with packet, mux and media histories all disabled. Evidence:
`artifacts/media-event-trace-observer-regressions.log`.

Combined all-off duplicate rejection and cadence/pause regressions pass; four
written cadence generations pass strict decode and packet/frame timestamp checks
with no missing/duplicate/regressing values. Evidence:
`artifacts/media-event-trace-all-off-media-regressions.log` and
`artifacts/media-event-trace-cadence-output-inspections.json`.
Nine pure workload-evidence validator tests pass, including bounded sender-log
tail preservation without private paths. Sender startup failures now preserve
exit codes, bounded owned log tails and the authenticated health snapshot;
fixture server log tails have session/CSRF/path/URL replacements before retention.

The added four-source AAC integration workload fails during startup on both new
native `0a3114dc38ac` and prior immutable native `02e07138f108`. The captured
control log identifies MPEG-TS ADTS-framed AAC rejected by the live MP4 muxer:
`Malformed AAC bitstream detected: use the audio bitstream filter aac_adtstoasc`.
The producer consequently exits with connection-reset error -10054. This is a
reproduced pre-existing format-handling bug, not a passed AAC integration result
or an attribution to optional event retention. It remains to be fixed separately.
Evidence: `artifacts/media-event-trace-disabled-four-stream-repeat.log`,
`artifacts/media-event-trace-aac-prior-build-control.log` and
`artifacts/replay-sender-failed-36495a31c2fbfde3.json`.

Native `0a3114dc38ac` / web `a0f360243279` built with unchanged pinned dependency
environment and bootstrap/manifest installation off. No native compiler/linker
warnings appeared; Sass/chunk warnings remain. Matching rebuilt consumers and
private-state regressions are required for new snapshot/state fields. Separate
package `build-vs2026-stable/deploy/media-event-trace-baseline-native-0a3114dc38ac`
verifies at 94 files / 177,798,996 bytes including symbols; output:
`artifacts/media-event-trace-baseline-native-0a3114dc38ac-package-verified.json`.
Host sampling, browser reporting/history and processing histograms still remain
to be mapped before full off/on overhead qualification. No production change,
main merge, remote push or user-owned backlog staging/edit occurred.

Normal-authenticated three-source video integration with packet, mux, media,
HTTP and SQLite tracing disabled passed in 112.879 seconds: two lifecycle phases,
10 sampled seconds each, continued ingest/processing, manual clips and DVR,
authenticated persisted health and clean shutdown. All 12 recorded outputs
passed strict decoding and packet timestamp inspection. Actual disabled coverage
flags and empty media feeds were asserted. Evidence:
`artifacts/media-event-trace-disabled-three-stream.log` and
`artifacts/media-event-trace-disabled-three-stream-{replay,workload}-{0,1}.json`.
Detection and browser playback were off. This validates the diagnostics work
piece's video path, not AAC transport, long-run stability or the full load gate.

### ADTS AAC live remux correction (2026-10-05)

Separate branch `codex/adts-aac-live-remux` resolves the four-source AAC failure
above. Exact pinned FFmpeg 8.1.2 `libavformat/movenc.c` disables AUTO_BSF when
EMPTY_MOOV is enabled. Our live `empty_moov+frag_custom+dash` output therefore
cannot rely on the automatic `aac_adtstoasc` used by ordinary MP4 recording.

An explicit, private per-live-output AAC bitstream filter converts referenced
packets without modifying shared input, recording, or capture payloads. Raw AAC
passes through unchanged. Filter state is freed on shutdown and reconnect.
When probing supplies no AudioSpecificConfig, a referenced actual ADTS packet
provides the configuration before empty_moov is emitted; presentation begins on
the next usable video RAP. There is no startup packet queue or guessed profile.
On the fixture this starts one second later (220 rather than 240 video frames).
Sources advertising AAC but never supplying configuration-bearing audio are not
covered: they remain waiting for configuration. No timeout/fallback for that
case is claimed by this change.

`LiveAacRemuxTests` exercises the production live mux with both ADTS MPEG-TS
(no probed ASC) and raw AAC MP4, verifies conversion payloads and packet metadata,
checks that input bytes remain unchanged, rejects malformed short ADTS, and
exercises filter reinitialization and live reconnect teardown. Both passed:
564 audio / 240 input video packets; 77 ADTS and 84 raw-AAC emitted partials.
Expected malformed-packet errors appear in the test log, not the positive media.
Strict video decode/timestamps passed (220 ADTS / 240 raw-AAC output frames,
no missing/duplicate/regressing timestamps), and separate strict audio decodes
passed without diagnostics. Output AAC is 48 kHz mono with 2-byte ADTS-derived
and 5-byte original raw extradata respectively. Evidence:
`artifacts/{adts,raw}-aac-remux-test.log`,
`artifacts/{adts,raw}-aac-remux-{video,audio}-inspection.json`.
The HEVC video-only optional-history/duplicate-decision regression also passed
(`artifacts/adts-fix-video-only-regression.log`).

The formerly failing normal-authenticated four-source replay now passes both
lifecycle phases in 128.609 seconds, including ten sampled seconds per phase,
continued ingest, manual clips/DVR, persisted authenticated health/history, and
clean shutdown. All 16 recorded video outputs passed strict decode/timestamp
inspection. Packet, mux, media, HTTP and SQLite observers were disabled and their
coverage asserted. Evidence: `artifacts/adts-aac-remux-four-stream-full-server.log`
and `artifacts/adts-aac-remux-{replay,workload}-{0,1}.json`.

Deployable candidate:
`build-vs2026-stable/deploy/adts-aac-remux-native-b3075dc3052e`, native
`b3075dc3052e`, web `47b30f8f2649`, verified 94 files / 177,825,108 bytes.
Manifest: `artifacts/adts-aac-remux-package-verified.json` (byte/identity coherence,
not runtime/ABI/stability proof). Builds reused the immutable dependency
environment; no package reinstall. Native compiler/linker warnings absent;
existing Sass/chunk warnings remain. No UI source changed.

Limits: positive AAC fixtures are AAC-LC mono / CRC-absent ADTS; HE-AAC, PCE,
multi-RDB/CRC framing, silent advertised audio, real RTSP reconnect timing,
browser playback, detection load, and long-run off/on overhead gates remain
unqualified. Four saved sources are not sixteen independent physical cameras.
This fixes a reproduced pre-existing failure, not all production Tapo stalls.

### Current-candidate AAC/MSE endurance validation preparation

Branch `codex/aac-mse-load-validation` adds
`Tools/Tests/summarize_replay_load.py`: descriptive CPU/HTTP and rolling filter
timings from completed replay reports, plus per-session browser counter deltas.
It rejects mixed builds/camera sets, excludes synthetic/other-build/hidden or
truncated/duplicate-player snapshots, keeps session replacements separate and
flags counter decreases even when the final counter subsequently exceeds the
first value. Four pure unit tests pass. Missing/nonfinite measurements remain
unavailable rather than being replaced with zero. The tool expressly does not
certify production stability, full-phase processing P95, or exact browser
measurement-window coverage.

A 30-minute current-candidate replay was started with 16 logical streams (the
four saved sources repeated four times, including four ADTS AAC transports),
CPU object detection capped at 1 fps, manual recording/DVR and strict recorded
video inspection. Five selective observers are disabled; functional integrity
and cadence machinery remains active. The real, normally authenticated browser
passed the positive playback gate and independent DOM checks showed all sixteen
players advancing with readyState 4, unpaused and no media error through roughly
six minutes. This is IN PROGRESS, not a completed endurance result. Output log:
`artifacts/aac-mse-16stream-selective-observers-off-30minute.log`; startup-view
proof: `artifacts/aac-mse-16stream-observers-off-start.png`.

The fixture owns its new DB/session/private cache and never alters production
configuration. Candidate/native/web identities are unchanged from the AAC fix.
No wake lock was taken, no build or fixture was restarted because of an
observation timeout, and no user-owned backlog changes were edited/staged.
After terminal completion, preserve its generic reports to unique evidence
names before starting another fixture. A comparable observer-enabled run and
actual representative-source/full-gate qualification remain outstanding.

Subsequent harness runs now record measurement start/end Unix milliseconds,
observed monotonic duration and individual health-sample receipt timestamps.
The summarizer uses server receipt time to restrict browser rows to that phase
when anchors exist, rejects invalid anchors, and explicitly labels older
unanchored reports instead of inventing their boundaries. This is NOT
retroactively applied to the currently running observer-off replay, whose Python
code was loaded before this harness edit. Six summary tests and all nine fixture
evidence validators pass. Two validator cases initially hit sandbox-only Temp
permission errors; the unchanged tests passed with access to their own temporary
directories. No running fixture or candidate binary was replaced.

### AAC/MSE endurance result: measurement completed, qualification failed

The observer-off run above is now terminal: 1,782 health samples covered 1,800
seconds, maximum sampling gap 1.299 seconds. Its overall test FAILED after
1,912.598 seconds because camera 2's manual recording exceeded the strict
inspection helper's 512 MiB input bound. This is not a demonstrated decoder
failure, and the large private recording was not copied. Final recording and
inference qualification did not complete. Preserved evidence:
`artifacts/aac-mse-16stream-observers-off-30minute-{replay,history,summary}.json`
and `artifacts/recording-inspection-failed-aa936bfe14db85f0.json`.

Descriptive process CPU (percentage of host capacity) P50/P95 was
28.887/39.760%; host CPU 57.564/76.795%; summary HTTP wall-time P95 27.473 ms.
There is no matched observer-enabled phase, so these do not establish overhead.
All sixteen main streams remained connected in every sampled server snapshot,
with initGeneration 0 and no timestamp-normalization rejection. Server
processing continued, but this does not prove browser presentation continued.

Eligible browser history covers approximately sixteen minutes, not the full
phase. All sixteen players recorded one restart/error and frame-counter resets.
At 06:42:00 UTC, a representative player restarted for 331 seconds of drift:
its playhead was unchanged while fragments and buffered end advanced. The WS
error was marked intentionalClose, so it is not evidence of a server disconnect.
Playback subsequently advanced and later froze again. An offscreen IAB renderer
or a genuine player fault remains unresolved; document visibility alone does not
distinguish those. The run is not continuous-playback or production-stability
proof. Next validation needs a continuously presented browser and bounded
long-recording inspection, rather than weakening either gate.

### Exact processing-task latency sample export

Branch `codex/processing-latency-sample-export` adds opt-in
`/debug/health?workSamples=1` behind the existing authentication policy. The
default response remains summary-only. Up to sixteen cameras export their
existing 1,024-entry completed-task ring chronologically, with an epoch and
first sequence from the same queue snapshot. A caller-owned fixed buffer keeps
allocation ownership out of the DLL boundary; no worker timing/storage is added.
Scope remains filter-stage wall time, excluding queue wait and decode.

The replay collector deduplicates overlapping sequence windows and archives
bounded raw durations. It reports exact full-phase nearest-rank P50/P95/P99 only
when every task between the per-camera snapshot cursors is accounted for.
Ring overruns, resets, camera-set changes, empty phases and collector capacity
limits cannot silently qualify a distribution. Boundaries are sequence cursors,
not fabricated per-task UTC timestamps. Seven collector tests, six summary tests,
nine workload-evidence validators and the native ring regression test pass.

Candidate native `6753258e104f`, web `cd53d3d7d020`, is frozen at
`build-vs2026-stable/deploy/processing-sample-export-native-6753258e104f`.
The verified package contains 94 files / 177,842,516 bytes; its manifest is
`artifacts/processing-sample-export-package-verified.json`. The native build has
no C/LNK warnings; existing frontend Sass/chunk warnings remain. Dependencies
were reused. The first normal-auth four-source, two-lifecycle smoke passed in
126.214 seconds with strict recordings and complete raw sample coverage for all
eight camera-phases. Evidence:
`artifacts/processing-sample-export-{replay,workload}-{0,1}.json` and
`artifacts/processing-sample-export-raw-{0,1}.json.gz`. This short test does not
qualify long-load overhead, independent physical cameras or browser playback.

A second four-source/two-lifecycle regression passed in 122.273 seconds,
including anonymous malformed-option rejection (403), authenticated malformed
option rejection (400), explicit `workSamples=0` omission of raw arrays, raw
sample collection, stats reset epochs and strict recordings. All eight
camera-phases retained complete exact task distributions. Evidence:
`artifacts/processing-sample-export-auth-smoke.log`,
`artifacts/processing-sample-export-auth-{replay,workload}-{0,1}.json` and
`artifacts/processing-sample-export-auth-raw-{0,1}.json.gz`.

### Bounded streaming inspection for long fixture recordings

Branch `codex/bounded-long-recording-validation` adds explicit harness opt-in
`WITNESS_TEST_LONG_RECORDING_INSPECTION=1`. It still validates fresh private-cache
ownership before inspection and retains the existing 512 MiB failure-copy cap.
The default small-probe inspector is unchanged. Long mode allows a local file
up to 8 GiB, fully decodes its first video stream with `-xerror` and a microsecond
output timebase, and independently streams every packet DTS and decoded-frame
timestamp. It does not sample windows or skip sections to achieve a pass.
Network demux protocols remain prohibited.

Each subprocess has a 1,800-second deadline, stdout lines at most 4,096 bytes
and at most two million records. Stderr is drained concurrently, retaining only
16 KiB while counting all diagnostic lines. Timestamp summaries use constant
memory instead of accumulating a full ffprobe JSON report. Missing timestamps,
duplicates, regressions, malformed output, decoder diagnostics, limits and
timeouts remain failures. Timed-out or malformed-output children are killed and
reaped using their owned process handles.

Eight tests pass, including timeout cleanup, stderr flood, strict failure cases,
a mocked 600 MiB input guard and a real 250,000-row timestamp subprocess. The
streaming inspector matches the existing strict inspector's timestamp evidence
on the actual H264/AAC fixture. Normal-auth four-source replay passed two
lifecycles in 121.622 seconds with all sixteen fresh video outputs inspected by
streaming mode. Evidence: `artifacts/long-recording-streaming-four-source-smoke.log`
and `artifacts/streaming-recording-{replay,workload}-{0,1}.json`.
Nine ownership/workload validators and four original small-probe tests also
pass. Candidate binaries and package identities are unchanged; this is test
harness work only. Actual >512 MiB/30-minute media, continuous browser rendering,
and the full matched observer overhead gate still require a new endurance run.

### Scheduling-delay recovery regression and presented-browser run

`codex/mse-scheduling-delay-regression` exercises the actual production MSE
composable against one 60-second callback gap and repeated 25-second callback
gaps. Both preserve suspension grace yet still recover a frozen visible playhead
once runnable watchdog time exceeds its threshold. All ten hidden-recovery and
visibility tests pass. This rules out permanent deadline erasure in these
simulated scheduling cases; it does not explain the renderer freeze or establish
how real browser throttling behaved. Large callback gaps can nevertheless stretch
the runnable-time-based recovery threshold into minutes of wall time. No player
source or running binary changed.

A new 1,800-second observer-off replay is running on native `6753258e104f`, web
`cd53d3d7d020`, with sixteen logical sources, CPU detection, manual clips/DVR,
exact task samples and bounded full-recording inspection. The normally
authenticated IAB is explicitly presented; its fixture-only layout was changed
to a 4x4 fullscreen grid with activity hidden so all sixteen videos lie inside
the viewport. Startup and independent subsequent DOM observations show all
players advancing at readyState 4, unpaused and error-free. This is an in-progress
test, not a completed endurance result. Evidence so far:
`artifacts/presented-browser-16stream-observers-off-30minute.log`,
`artifacts/presented-browser-16stream-grid.png`, and
`artifacts/presented-browser-16stream-{start-dom,progress-1,progress-2}.json`.
No wake lock, deployment mutation or production data/session was used.

`codex/browser-phase-coverage-audit` makes browser evidence boundaries explicit:
first/last eligible server-receipt timestamps, largest inter-snapshot gap, and
start/end offsets from the anchored measurement phase. A descriptive 30-second
boundary-observation check is explicitly not continuous-playback proof. Older
unanchored reports and single-row gap evidence remain unavailable, not guessed.
Eight summary tests pass, including a retained tail that misses twenty minutes
of a thirty-minute phase. This changes only post-processing, not the running
fixture or production history limits.

### Presented-browser 30-minute phase completed; recording inspection pending

The presented run completed its measured phase on native `6753258e104f`, web
`cd53d3d7d020`: 1,759 samples over 1,800.957 seconds, maximum gap 1.346 seconds.
All sixteen camera snapshots stayed Connected with main initGeneration 0 and no
timestamp-normalization rejection. Exact task collection is complete for every
camera. Filter-stage P95 spans 0.0172--0.0781 ms, explicitly excluding decode and
queue waits. Witness CPU P50/P95 (host capacity) was 29.035/38.372%; summary HTTP
wall-time P50/P95/P99 19.305/40.399/50.371 ms. These are descriptive, not a matched
off/on overhead result; the raw-sample request itself adds work to HTTP snapshots.

The eligible browser export retains 119 snapshots spanning 1,773.527 seconds,
starting 14.115 seconds into the phase and ending 13.316 seconds before its end;
maximum receipt gap 15.135 seconds. Every player's restart/stall/error delta is
zero, with no observed counter decreases. Independent DOM samples also show
advancing playheads without pause/media errors. This contrasts with the earlier
offscreen run, but does not establish causality: candidate instrumentation and
layout/presentation differ. Four 4K HEVC players dropped approximately 4.7--4.9%
of frames. Four AAC players show sampled latency P95 3.859--3.983 seconds and
occasional readyState below 3, despite no counted persistent stall or restart.
No claim of flawless presentation or production stability is made.

Completed-phase evidence was copied before further tests could overwrite it:
`artifacts/presented-browser-16stream-off-30minute-{replay,history,summary}.json`
and `artifacts/presented-browser-16stream-off-30minute-raw.json.gz`.
`artifacts/presented-browser-persisted-progress-23minute.json` is a bounded,
read-only inspection of this fixture's telemetry DB, not a production DB/session
copy. Default sandbox access hit private temporary-directory ACL errors; the
same explicitly owned read-only check succeeded with appropriate access.

The overall test is still running strict full-recording inspection. An owned
ffprobe process was observed after the harness's clean server-shutdown checks;
the finished browser tab was closed. Final recording/inference assertions and
the overall test result remain pending. No new fixture, build, wake lock or
production configuration change was introduced. The server/sample phase cannot
be relabeled as a passed full test until its remaining inspections complete.

`codex/recording-inspection-progress` adds optional atomically replaced progress
JSON for future long-inspection harness runs. It retains one numeric inspection
summary per completed output (private file paths removed), expected/completed
counts and an explicit incomplete state. A later decoder exception leaves prior
checks available; an inspection that returns `passed=false` cannot qualify the
final report. The final `passed` flag requires nonempty, complete, all-passing
results. Default callers remain unchanged. Ten workload-evidence tests pass,
including later exception, later failed result and all-success cases. The live
endurance fixture predates this edit, so no per-recording progress is invented
for it. Its owned decoder was observed moving from timestamp probing to camera
3 strict decode, which proves progress but not success of earlier recordings.

### Multiplexed fragment track timing audit (client sequence-mode lead)

`codex/fragment-track-timing-audit` adds a read-only, bounded fMP4 audit. It scans
top-level box extents without loading the media, maps ffprobe packet positions
to each fragment's mdat payload, and reports per-stream presentation envelopes,
inter-fragment track gaps and multiplexed track-end spread. Extended/zero-sized
boxes, malformed extents/packets, missing-track fragments and probe errors have
five regression tests. The underlying streaming probe limits rows, line length,
stderr retention and execution time; external protocols are disallowed.

Actual production-mux fixture outputs:

- ADTS input: 736 packets, 77 fragments, only 33 containing both streams;
  multiplexed end spread up to 92 ms. Video spans 0--11 s, audio 0--11.010666 s.
- Raw AAC input: 803 packets, 84 fragments, all containing both streams;
  end spread up to 50 ms. Both track envelopes span 0--12 s.
- Neither fixture shows a positive inter-fragment video gap; audio envelope
  gaps are at most 0.001 ms, within ffprobe's printed timestamp precision.

Evidence: `artifacts/{adts,raw}-aac-fragment-track-timing.json`. Stream indices
are not inferred codec identities, and envelopes are not full frame continuity
or perceptual playback proof. These short offline outputs do not reproduce the
thirty-minute AAC drift or rule out the looped sender's boundary behavior.

The client still deliberately uses `SourceBuffer.mode='sequence'`. The W3C MSE
specification warns about multiplexed tracks with misaligned end times in this
mode: https://www.w3.org/TR/media-source-2/ . Sparse audio fragments and measured
end mismatches make a real-browser sequence-versus-segments comparison worth
doing, but are not evidence that changing the default is safe. No player mode,
keyframe-seek safety, native muxing, deployed bundle or production settings were
changed by this audit. Late joins, reconnects and B-frame streams must be covered
before adopting any mode change. The long recording verification remains live
and pending, independently of this offline audit.

### Real-browser finite-fragment comparison: sequence versus segments

`codex/mse-fragment-mode-comparison` preserves an isolated test page and fixed-
asset loopback server. It exposes only the page and two fixture outputs, no
directory listing, arbitrary files, database, sessions or Witness routes. Media
is bounded to 16 MiB per file and frozen after SHA-256 verification. Changed
fixtures fail closed because codec bytes and the late-join RAP were verified
against these exact files. Two asset tests cover freezing, changed hashes,
missing files and oversize rejection. HTML JavaScript syntax and Python syntax
checks pass. Direct requests verified the three allowed routes and rejection of
five unlisted/query/traversal routes on the experimental server.

Four real-browser comparisons completed in the Codex in-app browser:

- ADTS full file: both modes buffered [0,11] seconds; 77 fragments.
- Raw AAC full file: both buffered [0,11.999999]; 84 fragments.
- ADTS late join at verified video RAP PTS 3s / fragment 21: sequence buffered
  [0,8] with timestampOffset -3; segments buffered [3,11] with offset 0.
- Raw AAC same late join: sequence [0,8.999999] / offset -3; segments
  [3,11.999999] / offset 0. Both appended 63 fragments.

Both players advanced through each remaining file without a media error during
12-second playback observations. Raw frame counters can include frames decoded
before the play promise resolves, so they are not interchangeable with full-file
strict frame counts. The finite experiment intentionally did not signal EOS;
readyState 2 at the media tail is expected exhaustion, not proof of a live stall.
The server and tab were closed after collecting the observations.

Evidence: `artifacts/browser-{adts,raw-aac,late-adts,late-raw-aac}-fragment-mode-comparison.json`
and `artifacts/browser-fragment-mode-comparison.png`. Each report includes every
append's ranges/offset and 49 playback snapshots. These observations do not
reproduce the AAC drift: nonaligned fragment boundaries alone did not cause a
timeline difference on full-file appends. Late-join rebasing confirms that raw
server PTS cannot simply be treated as browser time in sequence mode. No default
mode change is justified yet. This is a finite, unpaced H.264/AAC fixture, not the
production websocket player, long loop/reconnect, B-frame or hidden-resume test.
It does not qualify production stability or replace the active goal's longer
matched-observer and real-stream gates. The recording-verifier handle remains
live and was observed advancing to camera 6 strict decode.

### Paced three-loop AAC sender-clock audit

`codex/paced-replay-clock-audit` adds a bounded, read-only packet-clock audit:
separate clocks for at most 32 stream indices, positive-gap/overlap counts,
duplicate/backward DTS counts, duration sum versus elapsed envelope, and only
the largest sixteen discontinuities per stream. Three regression tests cover
padding overlaps versus backwards DTS, per-track isolation, bounded anomalies,
MPEG-TS side-data separators and malformed clocks. The existing bounded process
reader supplies line/row/stderr/timeout limits and local-only demux protocols.

Captured three paced loops of the same 12-second H.264/AAC source with the
endurance sender's `-re -stream_loop -map -c copy -f mpegts` options (finite loop
count and local-file output, not HTTP/network timing). The source MP4 has AAC
priming at DTS -0.021333 and 564 audio packets. The resulting TS contains 720
video / 1,690 audio packets. Video has two 21.289 ms loop-boundary gaps; audio
has two approximately 10.6 ms gaps. Neither track regresses or overlaps.

The existing production `LiveAacRemuxTests` executable successfully remuxed the
captured TS: 245 partials. It retains 700 video / 1,642 audio packets after the
documented initial audio-config/RAP wait. Output envelopes span video
0--35.042578 s and audio 0--35.053291 s. No video gaps, duplicates or regressions
are observed; audio gaps are at most 0.001 ms printed-precision rounding. Strict
video decode/timestamp inspection passes with 700 decoded frames and no decoder
diagnostics; separate strict audio decode exits zero without diagnostics. The
test's final intentional malformed-ADTS check emits its expected error, not a
decode failure of this output. No native code or deployed binaries changed.

Evidence: `artifacts/paced-aac-three-loop-input.ts`,
`artifacts/paced-aac-three-loop-{packet-clock,output-packet-clock,output-fragment-clock,output-decode}.json`,
`artifacts/paced-aac-three-loop-live-output.mp4` and
`artifacts/raw-aac-fixture-packet-clock.json`. This weakens the hypothesis that
ordinary loop timestamp boundaries alone explain the multi-second AAC browser
lag, but does not rule out HTTP delivery bursts, thirty-minute behavior or
production-player handling. The remux test consumes the captured file offline;
it does not reproduce live wall-clock arrival or sixteen-player resource load.
The long recording-verifier handle remains live and was observed at camera 7
strict decode; overall endurance recording assertions remain pending.

### Bounded playhead-position evidence retained across client/server health

`codex/mse-buffer-gap-evidence` adds passive scalar evidence identifying whether
the media element's playhead is inside a buffered range, usable headroom in that
range, or the next buffered start and distance across a hole. This supplements
the final buffered-end latency value: a large end-minus-currentTime alone cannot
distinguish a hole from playable media. Exact range-end exhaustion is not marked
buffered, and invalid/disappearing/overlapping or more than 64 ranges leave
position unknown rather than inventing a gap. No seek, restart, append mode,
buffer trimming or other playback decision changes.

The fields survive the TypeScript allowlist, rich sixteen-player POST compaction
and native authenticated-history scalar allowlist. Compaction also retains AAC
presence, SourceBuffer mode and timestamp offset, without transmitting URLs,
arbitrary JSON or full unbounded range lists. These are observed media-element
intersection ranges, not separate audio/video decoder readiness. Sparse history
can still miss a transient between samples, and old stored samples cannot be
retroactively classified.

Nineteen MSE/health tests pass; one optional external-export test is skipped.
Coverage includes true holes versus playable headroom/tails, malformed ranges,
the actual composable's one-second snapshot path, hidden recovery, rich payload
budget and causal snapshots. An initial test incorrectly compared a one-second
snapshot with the final tick's buffer end; it was corrected to compare values
from the same snapshot. The native payload test passes, including false/zero
gap fields, audio timeline context and exclusion of URLs/range arrays.

Pinned RelWithDebInfo build completed with native `648a3dfc9a0c`, web
`7c71d142b7f3`; no C/C++ or linker warnings appeared in this incremental log.
Existing Bootstrap/Sass deprecations and bundle-size warnings remain. No vcpkg
package rebuild was requested. Frozen package:
`build-vs2026-stable/deploy/mse-buffer-gap-native-648a3dfc9a0c`, 94 files,
177,843,662 bytes, with PDBs but no private DB/config/session. Evidence:
`artifacts/mse-buffer-gap-evidence-build.log` and
`artifacts/mse-buffer-gap-package-verified.json`.

Package verification proves byte/identity coherence, not runtime/ABI or stream
stability. A fresh authenticated stream smoke and actual browser-to-server
history collection with these new fields remain to be run. The old thirty-minute
candidate stays isolated in its original package and its recording-verifier
handle remains live; its measured phase cannot be attributed to this new build.

### Authenticated HTTP and real-browser retention of buffer-gap evidence

`codex/buffer-gap-http-history-validation` extends the real cookie/CSRF HTTP
regression with gap-position and audio-timeline scalars. The native loopback
fixture was rebuilt against the current payload allowlist. The test passed in
0.615 seconds: authenticated Alice/Bob submissions are stored under server-
resolved owners, Bob's subsequently revoked session cannot write, administrator
export retains both owners, graceful restart restores both rows, and all new
false/zero/negative-offset scalars survive. Injected URLs and full range arrays
remain excluded. Anonymous, wrong-port-cookie, disabled/missing-user and CSRF
cases retain their expected rejection. This is the shared production auth,
history and payload code in a disposable two-worker fixture, not the full camera
server. Build/test logs: `artifacts/buffer-gap-health-http-fixture-build.log` and
`artifacts/buffer-gap-authenticated-http-history.log`.

The browser fixture bundles the actual reporter/API with explicitly synthetic
player data. Two real Codex in-app browser tabs submitted their own independently
generated session IDs under the same browser-local ID, then exported history
through real port-scoped cookies and CSRF. Nine rows were durably exported with
the server-resolved fixture administrator and preserved gap fields. Stopping and
closing camera-8's reporter tab left its stored session available to camera-9's
tab. An injected URL never appeared in exported JSON. Both tabs stopped and
closed; the owned fixture stopped gracefully with exit zero and its temporary
databases were cleaned by its harness. No production authentication/configuration
or camera data was used.

Evidence: `artifacts/buffer-gap-browser-history-{active,disconnected}.json` and
`artifacts/buffer-gap-browser-history-disconnected.png`. The first esbuild attempt
hit sandbox child-spawn EPERM, so the prematurely started stale-module fixture
was stopped without browser use, the module rebuilt with appropriate access,
and a fresh disposable fixture used for all reported browser results.

Limits: this validates browser reporting/retention of synthetic fields, not
actual-stream detection of a gap. Available browser inventory contains one
in-app profile plus MCP Apps, so these are two sessions in one profile, not two
independent browsers/devices. Full-server browser-to-history stream evidence,
independent profiles and matched long-run performance gates remain open. The
separate endurance recording-verifier handle is still live, not restarted or
relabelled as a passed full test.

### Exclusive full-server test evidence and diagnostic-build lifecycle smoke

`codex/isolated-full-server-test-evidence` adds an opt-in
`WITNESS_TEST_ARTIFACT_NAMESPACE`. A validated lowercase, bounded name creates
an exclusive `artifacts/run-<name>` directory; existing names, linked roots and
path traversal are rejected. All reports, browser-history export, raw-sample
archive, recording progress/failures, shutdown dump and private temporary test
root use that directory. Default callers retain their existing paths. Twelve
evidence-validator tests pass, including untouched other-run sentinel files,
linked roots, namespace reuse and failure routing into the selected directory.

The diagnostic package native `648a3dfc9a0c` / web `7c71d142b7f3` completed two
authenticated full-server AAC replay lifecycles in 62.619 seconds. Each lifecycle
created and strictly inspected its fresh manual clip and continuous DVR output;
four outputs total passed full streaming decode/timestamp inspection. Both
recording-progress reports are complete and passing. Normal session/CSRF checks,
durable synthetic health history and clean server shutdown/restart checks passed.
Evidence is isolated under
`artifacts/run-gap-build-auth-smoke-20261005/full-server-{replay,workload,recording-progress}-run-{0,1}.json`
with console log `artifacts/gap-build-isolated-auth-smoke.log`. This smoke has one
saved AAC replay input, no live browser and no object-inference workload; it does
not prove actual-player gap telemetry, sixteen-stream performance or production
stability. The original long verifier remains live and was observed at camera
10 frame-timestamp inspection; its generic outputs were not overwritten by this
test. No source/binary changes beyond the previously packaged passive gap
instrumentation were introduced by the namespace harness changes.

### Actual MSE-to-server gap diagnostics validated on the diagnostic build

`codex/buffer-gap-stream-validation` preserves a completed authenticated replay
with the production dashboard/MSE player, native `648a3dfc9a0c`, web
`7c71d142b7f3`. The 180-second phase sampled 179 server snapshots over
180.409 seconds, maximum sampling gap 1.028 seconds. The positive browser-start
gate passed on actual advancing media before measurement began. Twelve eligible
history rows span 165.069 seconds, starting 14.087 seconds into the phase and
ending 1.253 seconds before its end; maximum receipt gap 15.013 seconds. Player
restart/stall/error counter deltas are zero, no counter decrease observed, final
readyState 4 and latency 1,033 ms. Its frame-count delta is 3,296 with 185 dropped
frames: this is not flawless presentation and not a clean load/performance
benchmark while the older fixture's recording inspector was decoding in the
background.

Forty-eight unique retained drift samples carry the real playhead-position
fields through the authenticated native history endpoint. All report the
playhead buffered; none report a hole or tail. Midphase read-only inspection
checked this exact owned fixture's loopback hostname before reading only player
telemetry, never auth sessions. AAC presence, timestamp offset and mode are
retained alongside headroom; an observed offset of -40.930625 seconds illustrates
why native timestamps must not be treated as the sequence-mode browser timeline.
Independent DOM observations show advancing playback without pause/media error.

The complete test passed in 258.349 seconds, including both fresh manual/DVR
outputs passing strict streaming decode and timestamp checks, normal auth/CSRF,
durable history, and clean server stop. The finished browser tab was closed. All
evidence is under `artifacts/run-gap-build-aac-browser-20261005/`, with
`full-server-{replay,workload,recording-progress}-run-0.json`, history, summary,
bounded midphase telemetry and DOM evidence. Console/screenshot:
`artifacts/gap-build-aac-browser.log` and `artifacts/gap-build-aac-browser.png`.

The replay summarizer now deduplicates retained drift observations by generation
and sample identifier, classifies explicitly observed buffered/gap/tail states,
and leaves legacy missing fields or conflicting repeats unknown. It never infers
a hole from the final buffered-end latency alone. Ten summary tests pass,
including duplicate observations, missing fields, conflicting repeats and bad
gap values. These are sparse media-element position snapshots, not continuous
playback or separate-track readiness proof. This one saved H.264/AAC input does
not qualify Tapo/Reolink network behavior, sixteen-camera contention, matched
observer overhead or independent browser profiles. The original sixteen-source
recording verification remains live and separate; no earlier incomplete test
was silently restarted, replaced or relabelled as passing.

### Under-buffered catch-up policy isolated (2026-10-05)

`codex/mse-underbuffer-catchup-regression` adds a control-policy regression
through the production MSE composable with in-memory media/socket primitives.
Starting at 1.5 seconds headroom, presentation pauses of 0.5, 1 and 2 seconds
while fragments keep arriving produce corresponding extra buffered-end latency.
With steady presentation/ingest restored, the existing 1.08x/1.03x hysteresis
returns to normal speed after 20, 26.25 and 38.75 simulated seconds respectively.
All three recover without a restart, stall counter increment or counted error.
The test also checks that low-ready playback never accelerates and that rates
remain limited to the intended values. Seventeen recovery, buffer-position and
drift-export tests pass from their required `web-ui` working directory; output:
`artifacts/mse-underbuffer-catchup-regression-web-cwd.log`. An earlier invocation
from the repository root failed to locate the drift test's cwd-relative source;
its separate log is retained and is not counted as a passing run.

This establishes that a short interruption can leave elevated latency for tens
of seconds with stable restart counters. It is not evidence that a decoder or
network interruption actually caused the sixteen-stream AAC latency spikes.
The fake media does not model decoded frames, audio/video track intersection,
browser scheduling or random-access dependency removal. Actual buffer-position
history and matched loaded browser runs remain necessary to separate those
mechanisms. Inspection also finds trimming after each eligible append, including
aggressive quota eviction retaining only one second behind the visible playhead;
that is a separate, unproven long-GOP dependency-removal lead, not a demonstrated
failure in the one-second-GOP replay. No production trim, seek, rate or restart
behavior was changed, and the existing verified diagnostic package is unchanged.

The original long verifier was polled by its live owned process handle and
observed at camera 14 frame-timestamp inspection during this work. This is
progress through inspection, not a terminal success or a per-recording verdict;
the full test is still pending.

### Real-browser long-GOP eviction silently freezes video (2026-10-05)

`codex/mse-long-gop-eviction-audit` adds a loopback-only, fixed-asset browser
comparison (`Tools/Tests/serve_mse_long_gop_eviction.py` and its HTML page).
It serves exactly the page and one synthetic 160x96 H.264 Baseline/AAC-LC
fragmented MP4, bounded to 16 MiB and frozen behind a SHA-256 check. It exposes
no application data, sessions, directory listing or writes. Both allowed routes
return 200; unlisted paths, query variants and traversal return 404. The reused
fixture hash/size guards pass two tests outside the sandbox; their first sandbox
invocation failed on temporary-directory ACLs, not an assertion.

Fixture SHA-256: `a132adc277cfb202bfae620cac72471430b0a9e25c10eb66bbfe9d166382021c`.
The fixed file is `artifacts/mse-long-gop-eviction-input.mp4`, generated using:

```text
ffmpeg -hide_banner -nostdin -n -f lavfi -i testsrc2=size=160x96:rate=24 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 -c:v libx264 -profile:v baseline -level:v 1.0 -pix_fmt yuv420p -g 120 -keyint_min 120 -sc_threshold 0 -bf 0 -c:a aac -ac 1 -b:a 48k -movflags +empty_moov+frag_keyframe+default_base_moof artifacts/mse-long-gop-eviction-input.mp4
```

The installed CLI reports Lavf63.1.100 / Lavc63.1.100; this synthetic CLI mux is
not production mux output. Keyframe PTS verified by ffprobe are 0, 5.021403 and
10.021403 seconds. Three actual browser sequence-mode MSE players append the
same bytes and seek to seven seconds. No removal retains [0,12.021332]; removing
[0,2] (five-second back buffer) retains [5.021402,12.021332]; removing [0,6]
(one-second aggressive back buffer) instead retains [10.021402,12.021332].

The first run observed clocks advancing but did not measure presentation, so it
could not establish whether video froze. The second run adds actual
requestVideoFrameCallback counts/mediaTime and getVideoPlaybackQuality counters.
After 3.011 seconds, all clocks are near 9.9709 with readyState 4 and no media
error. Control/normal have 61/62 presented callbacks and last mediaTime 9.979736;
the aggressive player has only five callbacks, last mediaTime 7.146402 and five
total video frames. Thus this short actual-browser fixture demonstrates silent
video freeze despite an advancing clock, not merely a changed TimeRanges value.
The current production watchdog's frozen-frame check only compares currentTime;
that would not detect this mechanism. Evidence:
`artifacts/mse-long-gop-eviction-presented-frames.json` and matching PNG.
The first, clock-only report remains separate as
`artifacts/mse-long-gop-eviction-browser.json`.

This does not prove quota eviction occurred on a Tapo production stream, nor
qualify all codecs, browser engines or long-GOP lengths. No real quota pressure,
production websocket/watchdog, reconnect, network load or decoder-slot contention
was involved. No production fix is claimed yet: safe removal boundaries and
presentation-progress recovery now have a concrete reproduction to target.
The native/web diagnostic package is unchanged.

### Dependency-protected MSE eviction candidate (2026-10-05)

`codex/mse-dependency-safe-eviction` implements a bounded 64-entry list of
successful independent first-part append-completion ends in the actual browser
timeline. These are deliberately **not seek targets** or exact RAP timestamps.
Both ordinary and quota-driven removal are capped at the older of the two
latest independent append ends behind the reference, retaining the current
decoding interval and an earlier boundary that removal may extend past. Hidden
players use the live edge as reference, not the frozen playhead. Missing,
insufficient, malformed or excessive boundary evidence permits no removal;
existing bounded queue/append watchdogs handle unrecoverable quota pressure
instead of deleting potentially required frames. Discontinuity, socket teardown,
restart and unmount all clear the list. Native packet metadata, audio handling,
keyframe-seek eligibility and catch-up rates are unchanged.

Twenty-one affected web tests and Vue/TypeScript checking pass. The production
composable regression feeds five-second independent intervals with seeking
explicitly unsafe, forces a QuotaExceededError and confirms removal caps at .25
seconds rather than six, retries the queued append, and neither seeks nor
restarts. Other tests cover a sixty-second GOP-like interval, unknown/bad
boundaries, hidden buffering and the prior transport/suspension recovery cases.
Log: `artifacts/mse-dependency-safe-eviction-tests.log`.

The real-browser comparison now imports the actual compiled pure policy and
records append-completion ends while appending the fixed keyframe-fragmented
synthetic file. At playhead seven, only one independent completed interval is
behind the playhead, so protection refuses the unsafe removal. After 3.010
seconds, protected/control/normal last presented mediaTime is 9.938069; protected
has 61 presented callbacks and 76 total frames. The unprotected one-second
removal again freezes at 7.146402 (five callbacks/frames) while its clock reaches
9.969195 and no media error appears. Evidence:
`artifacts/mse-dependency-safe-eviction-browser.json` and matching PNG. This
proves prevention in that fixture, not production Tapo causation or all codecs,
GOP structures, actual quota exhaustion or browser engines. The presentation
watchdog improvement remains a separate pending change.

Pinned incremental RelWithDebInfo build completed without dependency installation
or C++/linker warnings; existing Sass/Bootstrap and frontend chunk advisories
remain. Frozen package:
`build-vs2026-stable/deploy/mse-dependency-safe-648a3dfc9a0c-web-6f20fe1e81b0`,
native `648a3dfc9a0c`, web `6f20fe1e81b0`, 94 files / 177,844,232 bytes,
verified byte-for-byte by `artifacts/mse-dependency-safe-package-verified.json`.
The package contains no private database, session or server configuration.
An authenticated full-server three-minute real-browser synthetic long-GOP replay
and strict recording verification has been started in the exclusive evidence
namespace `artifacts/run-dependency-safe-browser-20261005/`; it is not yet a
terminal pass. The earlier sixteen-source verifier remains separate and live.

That separate candidate lifecycle has now completed successfully in 279.196
seconds. Its positive actual-browser gate passed before the measured phase;
179 snapshots cover 180.741 seconds with maximum gap 1.259 seconds. Twelve
eligible authenticated browser-history rows span 165.051 seconds, start 14.895
seconds into measurement, and end .789 seconds before completion; maximum receipt
gap is 15.006 seconds. Restart/stall/error deltas are zero with no observed
counter decrease, 3,953 video-frame delta and zero dropped-frame delta. All 48
unique retained playhead-position samples report buffered, with no gap/tail;
final readyState is 4 and latency 1,380 ms. Actual midphase DOM evidence shows
normal advancing playback at rate 1, currentTime 115.336522, retained range
[108.192622,116.714021], no pause or media error. Both fresh manual and continuous
recordings pass full strict video decode and packet/frame timestamp inspection
(6,182 and 6,194 inspected frames); authenticated history and clean server stop
checks pass. Recording progress is complete with two of two passing outputs.
The finished test browser tab is closed and the fixture cleaned up.

Evidence and summary are in the exclusive namespace above; console log
`artifacts/mse-dependency-safe-browser.log`; midphase DOM/screenshot
`artifacts/mse-dependency-safe-stream-midphase.json` and matching PNG. The complete
web suite also passes 68 tests, with one optional real-player export test skipped
(`artifacts/mse-dependency-safe-full-web-tests.log`). This is one synthetic small
H.264/AAC source, without object inference or actual quota pressure, while the
older HEVC recording inspector remains active. It does not qualify real Tapo
network behavior, high-resolution decoding, all codecs or observer-overhead
gates. The broader goal and the independent sixteen-source verifier remain open.

### Presented-frame recovery implementation and real-page iteration (2026-10-05)

`codex/mse-presented-frame-recovery` adds one cancellable presented-frame callback
per production MSE player. Finite mediaTime advancement resets progress even for
unchanging image content; callbacks from an old SourceBuffer generation cannot
seed the replacement. Discontinuity/reconnect/unmount clear the evidence. A
visible, playing, non-seeking, unsuppressed player with recent incoming fragments
recovers if its clock stays more than one second ahead of presented video for
over three seconds of runnable watchdog time. Scheduling suspension is excluded;
repeated delayed ticks still accumulate runnable time. Unsupported observation or
absence of any presented-frame baseline does not invent a presentation stall;
the prior clock/transport/append watchdogs remain. Missing-first-frame recovery
is not newly qualified by this change.

Visibility uses geometric intersection of the shared `data-playback-viewport`
(which contains the camera's own controls). Once video falls behind the clock,
up to five local hit-test points exclude an external covering dialog without
rejecting own labels/buttons. No per-frame geometry/hit-test work is performed
while presentation is caught up. Hidden documents, offscreen viewports, paused,
seeking and intentionally render-suppressed players are excluded. The causal
restart snapshot survives routine-event eviction. Presented mediaTime/count,
runnable stall time and geometric visibility pass the existing bounded scalar
allowlists and compact-report budget through authenticated native history.

The full final web suite passes 77 tests, with one optional export test skipped;
TypeScript checking and native ClientHealthPayloadTests pass. Regression coverage
includes advancing audio/media clock with frozen video, static-image timestamp
progress, hidden/offscreen/covered/paused/seeking/suppressed gates, delayed ticks,
generation replacement, viewport selection, callback cancellation and compact
health export. Twelve workload-evidence tests also pass. Logs:
`artifacts/mse-presentation-hit-test-web-tests.log`,
`artifacts/mse-presentation-viewport-tests.log`, and build/native test output.

Two earlier packaged variants used browser IntersectionObserver v2 occlusion
visibility. Their authenticated full-server lifecycles completed (288.752 and
273.944 seconds respectively), but real persisted history reports
presentationVisible=false while frames advance. Therefore they **do not qualify
the recovery feature**, despite the old playback-only harness passing. Their
unmodified packages and evidence remain separate: web `a785d444ede0` and
`2f5cc0830a97`, native `ccce3ef12e45`, namespaces
`artifacts/run-presentation-browser-20261005/` and
`artifacts/run-presentation-viewport-browser-20261005/`. The exact cause of the
browser's conservative occlusion classification is not established. These are
not recommended deployment candidates. No production data/authentication was
changed during either test; read-only midphase probes checked the exact owned
loopback hostname before reading only health payloads, never session data.

The harness now offers `WITNESS_TEST_PRESENTATION_MONITOR=1`: startup requires
fresh visible monitoring and finite presented mediaTime close to an advancing
clock, not just clock/frame-count evidence. Both earlier exports fail this
required gate. Wrong types, missing/disabled observation and non-finite/stale
mediaTime fail closed in unit tests. Default legacy callers are unchanged.

Corrected frozen package:
`build-vs2026-stable/deploy/mse-presentation-ccce3ef12e45-web-c7af85c866b9`,
94 files / 177,846,496 bytes, native `ccce3ef12e45`, web `c7af85c866b9`,
byte/identity verified by `artifacts/mse-presentation-hit-test-package-verified.json`.
Pinned incremental builds do not install dependencies and emit no C++/linker
warnings; pre-existing Sass/Bootstrap/chunk advisories remain. A fresh three-minute
synthetic long-GOP replay with the **required presentation gate** is now running
under `artifacts/run-presentation-hit-browser-20261005/`; its terminal outcome is
not yet known. Original sixteen-source recording verification remains separate
and live. Actual frozen-video recovery in a browser, broader codec/network/load
coverage and the matched observer-overhead gates remain unproven.

The corrected candidate has now completed the full-server lifecycle in 270.137
seconds with the strengthened presentation gate enabled. Its measured phase has
179 server samples across 180.483 seconds (maximum sampling gap 1.028 seconds).
Twelve authenticated retained browser rows span 165.059 seconds, beginning 14.330
seconds into measurement and ending 1.091 seconds before completion. There are
48 deduplicated presentation observations: all have visible geometric monitoring
and valid callback evidence, all 47 within-generation pairs advance presented
timestamps and callback counts, and none show a callback reset/nonadvance.
Maximum sampled clock lead over presented video is 62.378 ms and sampled runnable
stall time is zero. Restart/stall/error deltas are zero with no observed counter
decrease; video-frame delta is 3,954, dropped-frame delta zero, final readyState 4,
and final latency 1,361 ms. All 48 playhead-position observations are buffered.

Both fresh outputs pass full strict decode and packet/frame timestamp inspection:
manual recording 5,936 frames and continuous DVR 5,954, with no missing,
duplicate or regressing timestamps or decode diagnostics. Recording progress is
complete, two of two passing outputs. Authenticated diagnostics/history and clean
shutdown checks pass. Summary: `artifacts/mse-presentation-hit-test-summary.json`;
underlying replay/history/workload/progress reports remain in the exclusive
namespace above, console `artifacts/mse-presentation-hit-test-browser.log`.
The private fixture is cleaned; its browser tab is no longer in the session.

The replay summarizer now reports deduplicated presented callback evidence
separately from media-clock movement. Conflicting repeated samples stay unknown;
missing, boolean/non-finite numeric values or an unknown generation cannot invent
progress; generations are not stitched across resets. Thirteen summary regression
tests pass. These are retained observations, not continuous presentation or
occlusion proof. This healthy single small H.264/AAC replay neither deliberately
induces a freeze nor qualifies real Tapo traffic/high-resolution decoding or
observer overhead. The older sixteen-source audit is still live, currently in
continuous-recording frame timestamp inspection, so competing inspection CPU
also prevents treating this run as an overhead benchmark.

### Actual-composable browser freeze recovery audit (2026-10-05)

`codex/mse-browser-freeze-recovery-audit` adds a private fixed-asset loopback test
page that bundles the actual `useMseStream` and Vue modules. MediaSource,
SourceBuffer, decoding, timers, IntersectionObserver and presented-frame callbacks
are real browser implementations. The WebSocket constructor is replaced only
inside that test page by a paced finite synthetic transport; a MediaSource
subclass retains its real SourceBuffer for an explicit unsafe-removal control.
This deliberately bypasses the production dependency-safe trim policy to exercise
its fallback watchdog, without changing production code or authentication.

Fixture generation uses installed ffmpeg with lavfi testsrc2 160x96 at 24fps and
mono 48kHz sine, duration 35 seconds, libx264 baseline level 1.0, yuv420p,
`-g 240 -keyint_min 240 -sc_threshold 0 -bf 0`, AAC 48k,
`-movflags +empty_moov+default_base_moof -frag_duration 250000`. The resulting
`artifacts/mse-browser-freeze-gop10-input.mp4` SHA256 is
`a68f1cbe6b0985370e4e383a4fecbe52fa62a29b366fa1f2756b5a152fed8c0f`, pinned
in the server. ffprobe confirms keyframe PTS 0, 10.021403, 20.021403 and 30.021403;
full `ffmpeg -v error -xerror` video decode passes. The synthetic producer uses
six fragments for initial reserve, then one 250ms fragment per 250ms wall time,
with independent metadata at each forty-fragment boundary and no safe-seek flag.

Build the test bundle from `web-ui` using installed esbuild: entry
`tests/mseBrowserFreezeFixture.ts`, bundle=true, format=esm, platform=browser,
outfile `../artifacts/mse-browser-freeze-recovery.js`. Run
`python Tools/Tests/serve_mse_browser_freeze_recovery.py`; it binds only
127.0.0.1 with a random port, serves exactly three frozen bounded assets, and
automatically stops after fifteen minutes. Listed assets return 200; unknown,
query and traversal paths return 404. No application data/session route is served.
Open the printed URL, click Start player, then Arm unsafe eviction. The page
injects the fault at media time 11–14 seconds rather than depending on automation
round-trip timing. TypeScript checking of the fixture and Python syntax checks
pass. Generated fixture uses Lavf/Lavc63.1.100, not the production FFmpeg library.

The first manually timed attempt missed the permitted injection window and made
no removal. A first armed run reproduced the presentation-specific restart and
resumed callbacks, but its excessive repeated diagnostic serialization continued
running and later acquired a second restart; it is not a clean recovery pass.
Evidence is preserved as `artifacts/mse-browser-freeze-recovery-initial.json` and
matching PNG. The bounded rerun retains the causal restart once and ends only
after a replacement generation advances at least 24 callbacks and one second of
presented media time, readyState >=3, geometric monitoring active, and clock
within one second of presentation. It also requires exactly one restart/stall
and no errors; it does not rely on the page status text alone.

The actual bounded rerun passes. Removal ends at 10.038590 seconds while clock is
11.038590; the browser removes the future decode chain. Presented video freezes
at 11.313069 seconds while retained same-generation clock observations advance
by 1.957558 seconds. The production watchdog emits `presentationFrozenRestart`
with 3,013ms runnable stall, recent fragment age 217ms and no queued append.
Replacement generation 4 advances from 8 callbacks / mediaTime .354736 to
52 callbacks / mediaTime 2.563069, readyState 4, zero presentation stall. Total
restart/stall/error counters are exactly 1/1/0. Causal event occurs 4.123 seconds
after injection; callback resumption verification completes 6.124 seconds later,
including the normal reconnect backoff. Raw saved report independently satisfies
these assertions. Tested bundle SHA256:
`050a5322b83ef5d5ea9a7c016900189ad3d7425483b3b4ed5538ef4b1c61c43e`.
Evidence: `artifacts/mse-browser-freeze-recovery-verified.json`, matching PNG,
and `artifacts/mse-browser-freeze-recovery-summary.json`. The test player unmounts
after verification, its tab is closed and the owned asset server stopped.

This proves real-browser recovery from the deliberately injected dependency-loss
failure with the actual client composable. It is not a production WebSocket/
server test, quota-exhaustion reproduction, all-codec qualification or a long-term
stability/observer-overhead gate. The separately packaged healthy native replay
and earlier actual safe-eviction prevention evidence remain complementary.

### Concurrent cross-user diagnostic identity collision (2026-10-05)

`codex/client-health-owner-collision-regression` strengthens the actual HTTP
integration fixture: two different normal authenticated users concurrently submit
exactly the same browser/session identifiers and forged administrator ownership.
Both POSTs return 202; an immediate repeat by the first user returns 429 without
the second user sharing that limit. Administrator export contains both reports
with their independently authenticated user UID/username and the colliding
diagnostic labels intact. Revoking the second user's session denies further
submission without removing its evidence. Both owned reports survive a clean
fixture-process restart. Thus supplied diagnostic identifiers are not treated as
authentication principals or used to overwrite another user's history.

The current presented-frame fields (generation, clock, presented mediaTime/count,
runnable stall time and geometric visibility) also survive sanitization, durable
export and restart; arbitrary URLs/array ranges and forged ownership are excluded.
The pinned incremental ClientHealthHttpFixture rebuild completes without
dependency installation or compiler/linker warnings. The native real-cookie,
CSRF, disabled/missing-user, normal/admin and cookie-port tests pass in 0.606
seconds including the concurrent identity collision and process restart.
Log: `artifacts/client-health-colliding-owner-http-test.log`. Tests use disposable
loopback-only auth/history databases, not production sessions or cameras. These
are independent HTTP clients, not two actual browser profiles, and do not prove
the full live-server UI reporting/login/HTTPS path or browser throttling behavior.

### Configured PTZ write boundary (2026-10-05)

Inspection found that the direct connected Baichuan path did not check the
requested camera's PtzEnabled setting, and cached/linked HTTP transports could
also be selected before any current enabled-setting check. A transport's
capability is not permission to bypass the requested view's configuration.
`codex/ptz-configured-command-guard` checks the owned current configuration after
the existing per-camera authentication/CSRF boundary, before transport discovery,
lane selection or command dispatch. Movement/relative zoom/focus/preset go-to,
absolute zoom, preset save and preset delete reject a disabled requested camera.
Read-only capability queries are unchanged. An authenticated Stop bypasses only
this enabled-setting gate, so disabling PTZ after a move does not suppress its
safety Stop; per-camera authorization and the original-transport lease still apply.

Native production-SQL configuration regressions cover enabled, disabled linked,
missing, disabled-after-discovery and re-enabled views, including Stop permitted
after disable. Lease expiry, failed-Stop retry, ambiguous acknowledgements, linked
ordering, independent lanes, preset interlocks and shutdown still pass. Ten PTZ
client regressions pass. Logs: `artifacts/ptz-configured-guard-native-tests.log`
and `artifacts/ptz-configured-command-ui-tests.log`. The initial sandboxed Node
test invocation failed with spawn EPERM; the explicit approved rerun passed.

The full packaged native-server fixture now offers an opt-in
`WITNESS_TEST_PTZ_CONFIGURED_GUARD=1` check. It requires a synthetic configured
camera, grants the fixture user its camera group, verifies PTZ remains disabled,
and tests all five production write routes. Two full-server startup/shutdown
cycles pass in 46.801 seconds: anonymous and wrong-CSRF writes return 403,
authenticated disabled writes return 400 with the expected error, and a missing
camera remains unauthorized. Normal stream readiness, diagnostics authentication,
durable retained history and clean shutdown/restart checks also pass.
Evidence: `artifacts/run-ptz-configured-guard-20261005/full-server-ptz-guard-run-0.json`
and run-1; console `artifacts/ptz-configured-guard-full-server.log`. Fixture state
is disposed. No PTZ transport is configured, no physical motor command sent, and
no production configuration/authentication changed. This route test does not
prove connected/cached transport command suppression or Stop motor behavior;
the configuration helper and existing lease regressions remain narrower evidence.
Concurrent link reconfiguration during a movement remains an outstanding audit.

Coherent candidate:
`build-vs2026-stable/deploy/ptz-configured-81eef548334f-web-e656f4548d6b`, native
`81eef548334f`, web `e656f4548d6b`, 94 files / 177,847,008 bytes, byte/identity
verified by `artifacts/ptz-configured-guard-package-verified.json`. The pinned
incremental build installs no dependencies and has no C++/linker warnings;
pre-existing frontend deprecation/chunk advisories remain. No main merge,
remote push or production deployment occurred. The user's backlog remains dirty
and untouched. The separate sixteen-source recording verifier is still live;
this short isolated run is not the matched full-load performance gate.

### Retained PTZ Stop before replacement discovery (2026-10-05)

The existing route already excluded Stop from its no-client early return; the
suspected disabled HTTP-only early-return regression was not present. A separate
latency problem was confirmed by inspection: the Stop branch could run HTTP
replacement/linked discovery before calling the movement lease, although the
lease already retains the original moving transport. `codex/ptz-retained-stop-first`
adds `StopExisting`: without creating a lane it returns no value when no pending
movement exists, true on an acknowledged retained Stop, and false on failure
while preserving that original callback and scheduling the normal retry.

After existing per-camera authentication, speed/command validation and lane
resolution, the production route tries that retained Stop first. A handled
success/failure returns directly, avoiding discovery or a different transport.
Only a missing pending movement falls back to the previous emergency-Stop
discovery path. Disabled-setting Stop permission and the existing device-local
serialization/watchdog remain unchanged. No global/database reference is added
to retained callbacks; the class's private implementation layout remains intact.

Native regressions cover no-active-movement calls across 100 IDs without
allocating lanes, exactly-once retained Stop, failed retained Stop with retry,
and linked aliases resolved by actual production SQL using the same original
transport. Existing ambiguity, expiry, independent lanes, preset and shutdown
checks still pass. Ten client PTZ ordering regressions also pass. Logs:
`artifacts/ptz-retained-stop-native-tests.log`,
`artifacts/ptz-retained-stop-linked-tests.log`, and
`artifacts/ptz-retained-stop-ui-tests.log`.

The full packaged server passes two configured-disabled PTZ route/authentication
cycles and normal synthetic stream readiness, authenticated diagnostics, durable
history and graceful restart checks in 46.838 seconds. Evidence namespace:
`artifacts/run-ptz-retained-stop-20261005/`; console:
`artifacts/ptz-retained-stop-full-server.log`. This fixture configures no PTZ
transport, so active retained Stop is exercised by native callbacks/SQL tests,
not a full HTTP-to-firmware movement. Exact physical motor stopping time,
replacement-discovery latency under real firmware and concurrent link edits
remain unverified. No physical camera command or production setting was changed.

Verified candidate:
`build-vs2026-stable/deploy/ptz-retained-stop-194a9b4dc0a7-web-9d19c738914b`, native
`194a9b4dc0a7`, web `9d19c738914b`, 94 files / 177,848,032 bytes. Manifest:
`artifacts/ptz-retained-stop-package-verified.json` (byte/identity coherence, not
ABI or production stability proof). Pinned incremental build has no dependency
installation or C++/linker warnings; existing frontend advisories remain. No main
merge, remote push or deployment occurred; user backlog edits remain untouched.

### Generated dashboard visibility regression (2026-10-05)

Inspection found that hero7, balanced8 and grid16 filled their slots from the
priority list without consulting saved visibility. Switching from a custom
layout could resurrect hidden cameras, consume capacity and incorrectly claim
that a rectangular activity region was occupied. The generator now filters the
deduplicated ranking before assigning geometry; all three DashboardView callers
(display, regular collision checks and preset-change validation) pass the same
visibility set. Omitted visibility retains legacy all-camera behavior. Saved
priorities and custom rectangles remain unchanged; custom automatic-slot motion
candidate policy is intentionally not altered.

Pure layout regressions cover hidden high-priority IDs, capacity, priority order,
empty visibility and stale IDs across all presets. Compiled DashboardView setup
tests exercise preset transitions, the activity collision decision and restored
custom rectangles. The CameraGrid transition regression verifies that hiding,
replacing and hiding all visible IDs retains the exact group-camera collection,
clears controls for a hidden selection and leaves group membership unchanged.
The template still keys all group CameraCards by camera ID and hides them with
CSS/inert rather than removing players. These are component-state and source
checks, not a new rendered-browser or physical multi-camera decoding proof.

`node --test tests/*.test.mjs` from web-ui passes 79 tests with one optional
real-export fixture skipped. The pinned WitnessServer build, including vue-tsc
and production Vite output, completes with no dependency installation or native
compile/link warnings; existing Sass and chunk-size advisories remain.

Verified package:
`build-vs2026-stable/deploy/dashboard-visibility-194a9b4dc0a7-web-2d144ade7fe5`,
native `194a9b4dc0a7`, web `2d144ade7fe5`, 94 files / 177,848,179 bytes. Manifest:
`artifacts/dashboard-generated-visibility-package-verified.json`. This is byte
and identity coherence, not ABI or production stability verification. No main
merge, push, deployment, camera motion or production setting was changed. The
separate 16-source recording audit session 71718 remains live at the latest poll;
quiet output is not a passing or terminal result. The broader matched-observer
performance and real-browser gates remain open. User BACKLOG.md edits are not
part of this work piece.

#### Built-dashboard browser validation follow-up

The current built bundle was then exercised through computer-use in the existing
isolated `ptzDashboard.mjs --serve --layout --auto-slots` fixture, with the local
`artifacts/tapo9-rate-consistency-15minute-20261004.mp4` mux replay and a fresh
loopback origin. The fixture forwards no command to Witness or a physical
camera. All five synthetic IDs replay the same source; this is not a
five-independent-camera hardware-capacity measurement.

Through the real controls, hero7, balanced8 and grid16 each retained the original
three visible cameras while keeping two hidden. The grid DOM proves priority
order 13, 12, 10 fills the first three slots; hidden higher-priority IDs 15 and 14
do not consume them. Returning to Custom restored automatic-slot indicators and
the saved activity rectangle. Initially requesting hero7 with three visible
cameras and an actually overlapping activity rectangle was correctly refused;
after hiding cameras 12 and 13 in Arrange and placing activity at x=8,y=0,w=4,h=4,
the same preset succeeded with only camera 10 occupying its large slot. The
former false collision is absent and the activity panel remains beside it.

The bounded NDJSON evidence contains 26 five-second samples across 125 seconds.
Independent post-run assertions require all five player IDs and unchanged
startTime values in every sample, readyState=4, unpaused, no error, empty append
queues, zero restart/stall/error/integrity/corruption/suppression counters and
one opened / zero closed stream connections per camera throughout. Every
player's media clock advanced more than 100 seconds. Visible tile counts include
3 (regular/generated), 5 (editing) and 1 (saved/reselected preset). These checks
prove retained transport/player readiness across those transitions, not RVFC
progress for hidden players, physical camera behavior or broad codec capacity.

Artifacts: `artifacts/dashboard-generated-visibility-browser-20261005.ndjson`,
`artifacts/dashboard-generated-visibility-browser-summary.json`,
`artifacts/dashboard-generated-visibility-dom-proof.json`,
`artifacts/dashboard-generated-visibility-grid-proof.png` and
`artifacts/dashboard-generated-visibility-activity-proof.png`. The owned browser
tab was closed and fixture session 6113 stopped after capture. The existing
native194a9b4dc0a7/web2d144ade7fe5 package remains the candidate; this follow-up
does not change runtime code. At the separate long recording audit check,
session 71718 remained live and owned strict-decode PID 27180 showed a new stage
process; the audit is not yet terminal and is not claimed to pass.

### Message and timer header isolation (2026-10-05)

The existing vcperf trace `artifacts/build-profile-20261004-174504/build-timetrace.json`
summarizes 77 translation units with 500.269 aggregate frontend / 266.830 backend
seconds (not elapsed build time). GlobalContext.h appeared in 35 visits with
38.907 inclusive seconds; Messages.h in 17 visits with 24.378 inclusive seconds.
These are nested costs and must not be added or interpreted as a predicted
saving. Inspection showed Messages.h and TimerWorker.h import GlobalContext.h
without using its types. Removing both imports isolates worker/message consumers
from the server context graph. Messages.h explicitly imports string. Recording.cpp,
which actually accesses Context, now explicitly imports GlobalContext.h. The
first build intentionally exposed that previously hidden implementation dependency;
the corrected full build completes without native compiler/linker warnings.
No message fields, constructors, worker semantics or DLL layouts are changed.

The opt-in `MessageHeaderIsolationTests` target compiles Messages.h and TimerWorker.h
first, without the server Crow PCH or an explicit Crow/SQLite dependency. It checks
all message inheritance declarations and representative clip/motion/record/reconnect
values; its native executable exits zero. Adding the target reran CMake using the
same immutable dependency environment
`6b27239cefa9921f3d82643ccc48e50ce241b30f5ea7ce1761240cb59532dc61`, with manifest
installation/bootstrap still disabled. No package rebuild/install occurred.

A separate no-PCH MSVC 14.51.36231 `/EP /std:c++20 /utf-8 /D_WINDOWS` probe uses
that test source, repository Source/Camera/Common include directories, pinned
vcpkg include/opencv4 and compiler/SDK include directories. The control adds
`/FIGlobalContext.h`, modeling the removed import without modifying source or
restoring it in production. Generated preprocessed output sizes are 10,092,485
bytes (context control) and 4,959,108 bytes (isolated): 50.86% less text. Summary
and hashes: `artifacts/message-header-isolation-size-summary.json`. This is not
a token-count, compile-duration or full-build-speedup measurement. A controlled
compile timing run is deferred while the independent recording audit is active.

The zero-camera full-server lifecycle smoke passes in 15.833 seconds, including
normal authenticated health/history, graceful restart and persisted history;
namespace `artifacts/run-message-header-isolation-20261005/`. It is not a
camera-loaded shutdown, stream or performance gate. Production Vue/typechecking
build also completes; existing Sass/chunk warnings remain. Verified package:
`build-vs2026-stable/deploy/message-header-3d9183607607-web-99d827777842`, native
`3d9183607607`, web `99d827777842`, 94 files / 177,848,179 bytes. Manifest:
`artifacts/message-header-isolation-package-verified.json` (byte/identity proof,
not runtime ABI or production stability). No main merge, push or deployment;
user BACKLOG.md edits remain unstaged and untouched.

#### Reusable no-PCH message-header cost probe

`scripts/measure-message-header.ps1` takes explicit CompilerRoot, SdkIncludeRoot,
InstalledRoot and 1..10 repeats (default 3). It preflights paths before creating
a fresh GUID-named artifact directory, preprocesses the isolated/control cases,
then alternates their object-compilation order. It never configures CMake,
installs dependencies, deletes build output or writes into the supplied toolchain
and installed-package roots. `/Bt+` compiler phase logs, generated objects,
preprocessed text hashes and scalar elapsed-run results are retained privately.
The control force-includes GlobalContext.h; it models the removed import rather
than checking out and rebuilding an earlier revision.

The final command used CompilerRoot
`C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.51.36231`,
SdkIncludeRoot `C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0`,
InstalledRoot `build-vs2026-stable/vcpkg_installed/x64-windows-witness-v145-release`
(absolute path when invoking), and Repeats=3. Flags use C++20, utf-8, `/EHsc`,
`/W3 /WX`, `/O2 /MD`, NDEBUG and Windows-10 target macros. Compiler/SDK/package
headers use `/external:I /external:W0`, matching the CMake system-header policy;
project warnings remain fatal. The first prototype lacked `/EHsc` and produced
C4530; a subsequent strict run exposed vendored Crow warnings before correcting
system-header classification and target macros. Those incomplete/warning runs
are not used as the successful measurement.

Final evidence directory:
`artifacts/message-header-probe-f9bfb4198c304fe49f16f8600f836f6d/`. Six object
compilations exit zero. Context-control elapsed seconds: 7.646477, 7.636336,
7.837583; isolated: 2.601541, 2.598234, 2.584973. Medians are 7.646477 and
2.598234 seconds. Preprocessed text is 10,229,543 versus 4,931,667 bytes under
these flags; unlike the earlier `/EP` smoke it also defines NDEBUG and Windows
target macros. These measurements include host scheduling/cache effects while
the independent recording audit is active. They are not CPU-work figures,
PCH-enabled server compile timings, full-build speedups, runtime improvements
or ABI compatibility proof. Invalid compiler-root, SDK-root and repeat=0 inputs
were each rejected before creating another artifact directory.

The probe changes no runtime source; the existing verified
native3d9183607607/web99d827777842 message-header package remains the deployable
candidate. No main merge/push/deployment or dependency mutation occurred. The
recording audit handle 71718 is still live, with its owned Python PID42428 and
child timestamp-inspection ffprobe PID10200 observed after the prior decode
stage; this is verified continued work, not a terminal pass.

### Load harness HTTP timing scope correction (2026-10-05)

Inspection found the replay harness's `httpMs` timer stopped after JSON parsing,
raw processing-sample collection and sample sanitization. When raw work export
was enabled it also timed the larger `workSamples=1` response, yet the summarizer
labelled the result `summaryHttpWallMs`. This is not suitable evidence for the
ordinary summary HTTP P95 gate.

`health_request_timing.sample_health` now explicitly requests authenticated
`/debug/health?workSamples=0`; raw phases separately fetch `workSamples=1` and
`workSamples=0`. Both timers stop immediately after the request wrapper returns
the fully read response body, before any JSON parsing or collector work. Plain
phases make one request. The helper rejects HTTP failures, invalid intervals,
changed build/camera membership between paired responses and raw work samples
unexpectedly present in the ordinary response. The raw processing collector and
its sequence/epoch/overrun qualification remain unchanged. Two requests per raw
sample increase measurement traffic and must be kept identical in both phases
of a future paired observer test; they are not a free probe.

Samples carry explicit HTTP scope labels and separate summary latency. The
summarizer emits ordinary-summary P50/P95/P99 only when every sample has a
valid, finite, nonnegative value with the exact ordinary-summary scope. Missing,
partial, nonfinite, boolean or mis-scoped data remain unqualified/null, not zero
or partial-window percentiles. Existing `httpMs` is preserved under
`healthCollectionWallMs`, with its legacy collection ambiguity stated. Historical
exports are not retroactively relabelled as clean request-only measurements.

Regressions: 5 timing-helper, 15 replay-summary, 12 workload-evidence and 7
processing-collector tests pass. Timing tests verify clock reads precede JSON
decoding, one-versus-two request behavior and fail-closed errors. Workload tests
required an escalated rerun after sandbox Temp-directory ACL errors; unchanged
tests then passed. A real full-server fixture with one paced H.264 source,
raw collection and two 10-second lifecycle phases passes in 33.880 seconds.
Evidence: `artifacts/run-http-timing-isolation-20261005/`. First-phase ten samples
all carry the ordinary scope; summary request latency ranges 1.109..15.833 ms,
raw request latency 1.542..23.960 ms and the exact processing distribution is
complete. These are functional short-run checks while another inspection is
active, not a controlled load, playback or overhead gate.

A corrected descriptive summary of the earlier 16-source OFF run is preserved
separately as
`artifacts/presented-browser-16stream-off-30minute-summary-http-scope-corrected.json`.
Its 1,759 legacy samples have zero qualifying summary timings. The old 40.399 ms
P95 belongs to health collection (including raw export/parsing/collection), not
the ordinary-summary P95. Original evidence files are unchanged. This reinforces
the need for new matched same-candidate observer phases with correct timing.

No runtime source or packaged bytes changed; native3d9183607607/web99d827777842
remains the verified deployable candidate. No main merge/push/deployment or
production setting changed; user backlog edits remain untouched. The independent
recording audit handle 71718 remains live at the latest poll with no terminal
result; the overall goal and broad gates remain open.

### Scalar stream collection and completed recording audit (2026-10-05)

Branch `codex/scalar-stream-health-snapshot` introduces
`LiveOutputStream::GetStreamingSummary()`. It copies the existing scalar counters
and configuration under the stream mutex, then returns before allocating,
sorting or copying media-event, packet, fragment, segment and anomaly rings.
The terminal operational-status collector uses this path. Both existing
`GetStreamingDiagnostics()` modes retain their previous behavior, including
the media feed in the legacy `false` mode; HTTP incident exports are unchanged.
This is groundwork for a cheap cached sampler, not that sampler's completion.
No stream timestamps, packet bytes, presentation controls or authentication
behavior change. The exported Camera method requires the matched server and
Camera DLL package; no class data layout was changed.

The actual-mux `MediaTraceObserverTests` additionally require empty summary
rings, matching scalar counters/configuration and a subsequent unchanged legacy
media feed. H.264 and HEVC pass with `WITNESS_MEDIA_TRACE=0` and `1`; each compares
retained/disabled byte output, durations and corruption/recovery controls.
Sandboxed HEVC fixture initialization failed before exercising this change;
the unchanged four invocations pass outside the sandbox. Both logs are retained
as `artifacts/scalar-stream-summary-regressions*.log`.

The native server and regression target build completed successfully without
native compile/link warnings. Existing frontend Sass deprecation and chunk
advisories remain. The actual authenticated loopback server test passes two
10-second H.264 replay/lifecycle phases in 33.315 seconds, with fresh private
state, raw processing samples, ordinary HTTP timing and history after restart.
Evidence: `artifacts/run-scalar-stream-summary-20261005/`. This short functional
test is not evidence of a measured performance improvement or production
stability. Manifest/bootstrap remain disabled and dependencies were reused.

Verified deployable package:
`build-vs2026-stable/deploy/scalar-summary-e7df4d7f9adf-web-477ce480fde8`, native
`e7df4d7f9adf`, web `477ce480fde8`, 94 files / 177,848,691 bytes. Manifest:
`artifacts/scalar-stream-summary-package-verified.json`. It contains matching
binaries, symbols, web assets and models, not private databases or credentials.

The earlier 16-source OFF recording audit (handle 71718) has now terminated
successfully: one test in 15,605.887 seconds, including its 30-minute measured
phase and subsequent whole-recording inspection. Its preserved workload is
`artifacts/presented-browser-16stream-off-30minute-workload.json`: 16 manually
triggered clips and 112 continuous recordings (128 inspections), all strict
decode, packet DTS and decoded-frame PTS checks passed, with completed inference
evidence for every source. Continuous row evidence is aggregated by camera;
the inspection list identifies individual recordings. The disposable fixture
directory was cleaned up by the successful test. The measured candidate remains
native `6753258e104f`, not this new candidate. Recorded-output integrity does not
prove perceptual smoothness, real-camera capacity or zero live presentation
stalls. The separate browser evidence and corrected HTTP timing limitations
still apply; new matched same-candidate OFF/ON phases remain required.

No main merge, remote push, production change or user backlog edit/staging was
performed. The overall goal remains active.

### Background cached health summary (2026-10-05)

Branch `codex/cached-health-summary` moves the default `/debug/health` and
key-scoped `/api/v1/diagnostics/health` collection into a listener-owned background
sampler. It starts after Crow becomes ready and collects immediately, then waits
two seconds after each completed collection. It cannot queue overlapping periodic
collections. Stop interrupts the interval and joins any current collection before
camera transports or processing queues can be torn down. Lifecycle Start/Stop
have one owner; concurrent HTTP readers only acquire an immutable owning pointer.

Ordinary requests retain the existing administrator/session or API-key scope/IP
authorization checks before reading the cache. Authentication, accounts,
revocation results and request objects are never cached. Delivery copies the
serialized summary without parsing JSON, gathering counters or walking cameras
on the request thread. A top-level `cache` object reports current `ageMs`,
`intervalMs` (2000), `stale` (age greater than 6000 ms), cumulative collection
failures and whether the most recent collection failed. No valid sample returns
503 with Retry-After 1 and failure metadata. Failure keeps the last valid sample
with its original timestamp, rather than manufacturing fresh zero values.

Retained payloads are capped at 2 MiB. This bounds the stored summary, not every
transient JSON allocation during collection or response serialization. Host,
camera, processing, HTTP/route and SQLite counters remain available. Scalar
collection omits stream media-event rings, slow HTTP/active request details and
slow SQLite query details. Coverage explicitly says those feeds are omitted.
HTTP/SQLite legacy snapshot defaults still preserve incident details; their new
scalar modes do not consume or reset the underlying rings.

`details=1` requests the bounded incident feeds on demand, while `workSamples=1`
requests bounded raw processing samples on demand. They may be combined. Both
options accept only `0` or `1`; malformed options remain checked after auth.
The manual Health view now explicitly requests `details=1`, preserving its
FFmpeg/HTTP/database investigation feed. Default `workSamples=0` remains a cached
scalar request. `server.requestSequence` identifies the collection, not an HTTP
request: repeated cached responses intentionally share it. Existing sample UTC,
monotonic collection anchors, duration, native/web identities and process
instance identifiers describe that collection, not delivery time.

The cache native regression covers immutable repeated reads with no recollection,
interrupted waits, first-sample failure and recovery, valid-shaped oversized
payload rejection, preservation of a pinned reader after Stop, and joining an
actively blocked collector before releasing its captures. Evidence:
`artifacts/cached-health-cache-final-regression.log`. All four HTTP/SQLite observer
on/off combinations pass the prepared-query and request-accounting regressions,
including scalar counter preservation and non-consuming incident reads:
`artifacts/cached-health-server-observer-regressions.log`.

Actual authenticated full-server loopback/restart validation passes against final
native `2213c5ef8142` in 38.853 seconds, using a disposable database and one paced
H.264 input. Both phases show eight repeated requests sharing one sample, later
background sequence advancement, incident on-demand responses, and immediate
403s for cached and detailed requests after disabling the fixture account.
Re-enabling that private account restores access. Raw samples, clean shutdown,
restart and durable browser history remain covered. Evidence:
`artifacts/run-cached-health-summary-final-20261005/`. An earlier fixture assertion
incorrectly required a media-event field on unavailable streams; its failed
artifacts remain under `run-cached-health-summary-20261005/`, and the corrected
intermediate run passed in 36.750 seconds. These tests do not inject stalled
collection at the actual HTTP endpoint, validate API-key revocation specifically,
exercise independent browser profiles or prove loaded overhead/stability.

Web regressions: 79 pass, one optional real-player export test skipped; Vue type
checking and bundle build pass. Python HTTP timing (5), replay summarization (15)
and exact processing collection (7) tests pass. The native build reused the
immutable managed dependency environment; no package installation occurred.
Frontend Sass deprecations/chunk advisories remain a separate build-health task.

Verified package:
`build-vs2026-stable/deploy/cached-health-2213c5ef8142-web-b301018e1023`, native
`2213c5ef8142`, web `b301018e1023`, 94 files / 178,031,485 bytes. Verification:
`artifacts/cached-health-summary-package-verified.json`. No private fixture state,
credentials or test executables are included. This verifies matching package
bytes/identities, not runtime ABI or production stability.

Matched same-candidate 30-minute OFF/ON replay phases and ordinary summary HTTP
P95 qualification still remain; the earlier 16-source audit used an older
candidate and cannot be substituted. The periodic cache runs in both phases;
selective observer-off is not all functional diagnostics-off. No main merge,
remote push, production deployment or user backlog edit/staging was performed.
The full goal remains active.

### Matched cached-summary endurance attempt and profile guard (2026-10-05)

Validation branch `codex/matched-cached-health-load` uses the frozen verified
native `2213c5ef8142` / web `b301018e1023` package, not subsequently rebuilt output.
An initial observer-off attempt accidentally selected the 256x144 synthetic H.264
fixture. Independent DOM inspection caught that lighter workload. Only its
verified disposable server process group was signalled with CTRL_BREAK; the
harness then terminated with the expected connection-refused failure and cleaned
up its senders/private state. Its log and startup proof are preserved under
`artifacts/matched-cached-health-off-20261005*`. This attempt is excluded from
overhead/endurance qualification, not counted as a passing or unexplained crash.

The corrected OFF attempt is live on exec handle 58671, with normally
authenticated loopback browser `http://127.0.0.1:62518/` in owned IAB tab 51. Its
exclusive evidence directory is
`artifacts/run-matched-cached-health-off-20261005-v2/`, and log
`artifacts/matched-cached-health-off-20261005-v2.log`. All five selective observer
switches (`WITNESS_HTTP_TRACE`, `WITNESS_SQLITE_TRACE`, `WITNESS_PACKET_TRACE`,
`WITNESS_MUX_TRACE`, `WITNESS_MEDIA_TRACE`) and corresponding test mode checks are
0. Functional integrity/cadence and the new background cache remain active.
The test measures 1800 seconds after its presented-playback gate, using CPU
object detection capped at 1 fps, manual recording plus DVR, exact raw task
samples and correctly scoped ordinary summary HTTP timing. Bounded whole-file
recording decode/timestamp inspection is enabled after the measured phase.
The ON phase must wait until that inspection is terminal to avoid CPU contention
from the audit contaminating the next measured phase.

Sources, repeated four times in this order:
- `artifacts/tapo8-rate-consistency-15minute-window1.mp4`: H.264 1280x720, no audio;
- `artifacts/replay-qualified-hevc-1080p20.mp4`: HEVC 1920x1080, no audio;
- `artifacts/replay-qualified-hevc-2160p25.mp4`: HEVC 3840x2160, no audio;
- `artifacts/adts-aac-remux-fixture.ts`: H.264 160x90 with AAC.

This is sixteen logical paced loopback streams, not sixteen independent cameras,
raw original RTSP traffic, or sixteen full-resolution camera feeds. In particular
the AAC source is deliberately small codec/format coverage. The two qualified
HEVC recordings have repaired fixture clocks and are not evidence of original
camera pacing. These coverage limits apply to the eventual paired result too.

The browser is explicitly presented in a 4x4 fullscreen grid with activity hidden;
all sixteen videos fit its viewport. Startup DOM proof at 12:27:21 UTC confirms
the expected sizes, readyState 4, unpaused and no media errors. A subsequent
independent observation at 12:30:04 shows every playhead advancing by more than
ten seconds and still ready/playing. This is a checkpoint, not a completed
endurance result. Proofs:
`artifacts/matched-cached-health-off-v2-{start-dom,progress-1}.json` and
`artifacts/matched-cached-health-off-v2-start.png`.

The harness now accepts optional `WITNESS_TEST_EXPECTED_REPLAY_PROFILES`: a JSON
array with one exact `{videoCodec,width,height,audioCodec}` object per source.
`replay_profiles.py` qualifies ffprobe metadata before creating fixture state,
probes repeated source files only once, then checks actual available main-stream
health profiles at ingress readiness and after measurement. Future result JSON
records `replayProfiles`. Invalid shape, boolean dimensions, codec/resolution/audio
mismatch, diagnostic probe output or changed/duplicate live camera membership
fail closed. Five pure tests pass. The actual four-source/16-entry metadata guard
also passes outside the sandbox after the sandboxed probe failed. Evidence:
`artifacts/matched-cached-health-qualified-profiles.json`.

The running corrected OFF Python process predates the new guard import; its
inputs were independently qualified and an actual authenticated live summary was
checked using the same validator. It confirms all sixteen profiles, matching
native/web IDs, five observer modes off, cachedSummary mode, sample age 868 ms,
no collection failures and not stale. Evidence:
`artifacts/matched-cached-health-off-v2-live-profile-proof.json`. Only the newly
generated disposable fixture session was used; no token was printed or exported.
The future ON invocation should pass the profiles array from the qualification
file and keep every other workload, visibility and package setting unchanged.

No native or web bytes changed for this validation piece. No wake lock, main
merge/push/deployment, production session/config mutation or user backlog edit
was performed. The corrected process was verified live on its handle at the
latest poll; no measured phase or audit terminal result exists yet. The matched
overhead gate and overall goal remain open.

### PTZ update query lifetime: prepared fix, validation deferred

While frozen-candidate replay handle 58671 continues its measured phase, source
inspection found `HandleCameraUpdate` retaining its `SQLiteDatabaseQueryInstance`
for `UpdateCamera` through subsequent `ReolinkClient::AutoDetect` network I/O.
The prepared-query mutex therefore spans slow discovery and can delay an
independent camera update; its diagnostic query-scope timing also includes that
network work. The separate `codex/ptz-update-query-lifetime` branch scopes the
same bindings/update/error response in a block ending before client discovery.
It changes no SQL selection, fields, authorization, credentials or motor commands.
This source change is not yet built, committed or declared validated: native
build and actual-server regression are deferred until the measured phase ends.
The frozen native2213c5ef8142/webb301018e1023 running package is unchanged.

`ptz_update_scope_fixture.py` prepares an opt-in actual HTTP regression, enabled
by `WITNESS_TEST_PTZ_UPDATE_SCOPE=1` with at least two fresh loopback cameras. A
fake loopback Reolink endpoint accepts only Login, pauses discovery for one camera
and requires a second camera update to return before releasing that login.
Anonymous and invalid-CSRF updates must not reach discovery. Release/cleanup is
bounded and performed even after assertions, and no physical or motor command
is sent. Three fixture-driver unit tests pass: independent scoped update, a
simulated retained-mutex negative case, and missing-camera rejection. These are
driver checks, not yet proof against the real Crow handler. The intended next
test is old frozen candidate (negative report expected), then the scoped-query
candidate (positive report required), using exclusive artifact namespaces.

Inspection separately found that editing a linked-camera graph mid-gesture can
change `ReadPtzLaneId` before a release request. `StopExisting` then searches the
new lane rather than the lane holding the original moving transport. The old
lane retains its ten-second safety stop, but immediate release and concurrently
dispatched commands across reconfiguration remain unresolved. Do not claim the
query-lifetime fix solves this routing issue. A reproduction with real production
SQL and retained callbacks is still required before choosing a configuration
fence/gesture-identity design; no physical movement was attempted during review.

Independent real-browser checkpoints at 12:36:32 and 12:46:39 UTC show all sixteen
playheads still advancing, readyState 4, unpaused and error-free. Evidence:
`artifacts/matched-cached-health-off-v2-progress-{2,3}.json`. Browser tab 51 remains
marked for continuation in the same visible fullscreen grid. The handle remains
live, with no measured-phase or audit terminal output yet. Do not launch ON or
another heavy workload while OFF is measuring, or while its subsequent audit
still consumes CPU. No wake lock or production setting was changed; the
user-owned backlog remains untouched.

### PTZ update query lifetime: actual before/after validation (2026-10-05)

The measured OFF replay phase completed before the native build or additional
server fixture started. Its replay, browser history and exact processing samples
are preserved under `artifacts/run-matched-cached-health-off-20261005-v2/`.
The 128-output recording audit remains live on handle 58671; the first complete
recording passed strict decode and packet/frame timestamp checks. This is not
the matched OFF/ON gate: ON must wait for the recording audit to finish.

The real Crow regression reproduced the retained-query problem against frozen
native `2213c5ef8142`: the independent camera update did not finish during the
one-second stalled discovery window, then both updates returned 200 after release.
Evidence: `artifacts/run-ptz-update-scope-before-20261005-v2/`.
The same regression against the scoped-query native `94d7d02b84d4` passed in
23.220 seconds; the independent update returned before discovery was released.
Both tests used fresh authenticated fixture databases and loopback media sources;
anonymous and invalid-CSRF requests were rejected before discovery. The fake
transport received one Login and zero motor commands in each run.
Evidence: `artifacts/run-ptz-update-scope-after-20261005/`.
The initial sandbox attempt failed before server startup while creating a private
fixture directory; it is not a product regression. Native configuration/SQL and
movement-lease tests passed; three fixture-driver tests and diff checks passed.
The pinned native build reused dependencies and completed successfully; frontend
Sass deprecation and chunk-size advisories remain. The linked-graph mid-gesture
release-routing issue described above remains unresolved by this scope fix.

Separate verified package:
`build-vs2026-stable/deploy/ptz-update-scope-94d7d02b84d4-web-f283df0a6816`,
94 files / 178,030,973 bytes. Byte/inventory/identity verification is recorded in
`artifacts/ptz-update-scope-package-verified.json`, not a general ABI/stability proof.
The frozen matched-load package was not changed. No main merge, remote push,
production deployment, wake lock or user backlog edit occurred.

A read-only privacy audit found no camera recording extensions, recording/capture
paths or artifact paths in tracked files, the index, or reachable local/cached
remote history. The media objects present are notification sounds; `.ts` matches
are TypeScript sources, not transport recordings. Real replay media remains in
ignored `artifacts/`; build packages are untracked. This checked local reachable
history and cached remote refs, not a new remote fetch or unreachable Git objects.

### Remaining PTZ link-edit release: reproduced, not yet fixed

On separate `codex/ptz-link-edit-release`, the opt-in native configuration test
`PtzConfigurationTests.exe WitnessServer/Source/Database.cpp --link-edit-release`
now exercises production lane SQL and actual retained callbacks. It starts a
gesture through view 11 linked to 10, changes view 11's link to 12, and checks
that release immediately stops the original transport. Current code fails with
`release after link edit missed original moving transport` (exit 1), confirming
the inspected gap without moving any camera. The ordinary configuration suite
still passes. This reproduction is uncommitted; the production fix/design and
concurrent reconfiguration coverage remain outstanding.

The current OFF phase summary is saved as
`artifacts/matched-cached-health-off-v2-summary.json`: native2213c5ef8142,
webb301018e1023, 16 replay streams, 1,744 samples, 1,800.944 seconds, maximum
sample gap 1.230 seconds. Ordinary authenticated cached-summary HTTP timings
exclude raw sample collection and parsing: P50 1.782 ms, P95 23.460 ms,
P99 26.209 ms. Witness host-capacity CPU P50/P95 are 28.192/38.459 percent.
All sixteen exact task distributions are present. These are descriptive OFF
results, not a passed matched overhead or production stability gate. Handle
58671 remains live auditing the 128 recorded outputs; ON has not started.

### PTZ configuration-fence prototype (unintegrated)

The lease now has a prototype shared control guard and exclusive `Reconfigure`
operation. Under the exclusive fence it resolves owned old lane IDs, ends all
query scopes, stops retained transports, then invokes the configuration commit.
A failed Stop prevents mutation and preserves retry. Independent control readers
remain concurrent; configuration mutation waits for all active guarded commands.
The callback-only relink test now passes using this primitive, including refusal
to commit after failed Stop, successful retry, and an update waiting for an active
control guard. Existing production-SQL configuration and movement-lease tests
also pass. No physical camera command was issued.

IMPORTANT: Crow camera-update/PTZ handlers do not yet use the new primitive.
The running server behavior is therefore not fixed by this prototype. HTTP-route
integration must guard lookup through dispatch, stop both affected old components
before link mutation, release the fence before slow discovery, invalidate stale
clients and reject stale discovery publication after a concurrent update. It must
retain the query-scope responsiveness regression. Normal unrelated metadata edits
should not unnecessarily stop motors. The prototype/source/tests remain uncommitted
on `codex/ptz-link-edit-release`; no deployable server was rebuilt from this work.

### PTZ configuration fence: HTTP integrated and stale discovery tested

The prototype is now integrated into camera updates and seven PTZ handlers
(movement/presets, position, zoom read/write and preset list/edit/delete).
After authentication, PTZ configuration lookup and transport dispatch share a
read guard. Camera updates exclusively resolve owned configuration/lane snapshots,
stop affected old components before relevant PTZ changes, execute the scoped SQL
update and invalidate the changed camera's cached client. Failed Stop returns 409
without mutating configuration. Unrelated metadata changes do not stop motors.
The exclusive fence is released before discovery; client publication subsequently
checks current owned settings and link identity under a shared guard and cannot
overwrite a newer edit or a valid lazy-discovery client. Incomplete new credentials
no longer retain an old client after a relevant change. Query locks do not span
Stop or discovery I/O. PTZ readers remain concurrent across independent lanes;
configuration writers can wait for active guarded PTZ transport calls.

Pinned native build `c6994318de6c` completed successfully with reused dependencies;
frontend Sass/chunk advisories remain. Native production-SQL relink/fence tests,
ordinary configuration tests and movement-lease tests pass. Actual Crow regression
`run-ptz-reconfigure-after-20261005-v2` passed in 23.160 seconds: another camera's
update completes during a held discovery Login, concurrent disable completes,
and the delayed client is not published (subsequent position read returns 400).
The old `94d7d02b84d4` build fails the same fixture, returning 200 from the stale
position client (`run-ptz-stale-discovery-before-20261005-v2`). Initial attempts
were invalid because the fixture omitted camera permissions; those failures are
not product evidence. The fixture now installs the normal camera mapping. One
Login and zero motor commands were sent by the passing run; authentication and
invalid-CSRF rejection precede discovery. Relink during an actual HTTP motor
command, all physical transports, timing under concurrent configuration writes,
and production stability remain unproven; callback tests are not substitutes.

Verified separate package:
`build-vs2026-stable/deploy/ptz-reconfigure-c6994318de6c-web-c18f088c51d8`,
94 files / 178,127,741 bytes; manifest `artifacts/ptz-reconfigure-package-verified.json`.
No private media, databases or configuration is packaged or staged. No main merge,
remote push, production deployment or user backlog edit occurred. Frozen matched
load candidate 2213c5ef8142/webb301018e1023 is unchanged; recording audit handle
58671 is still live and the ON phase has not started.

The packaged candidate also passed the actual disabled-camera PTZ/configured
policy fixture across two shutdown/restart lifecycles in 39.004 seconds:
`artifacts/run-ptz-reconfigure-disabled-guard-20261005/`. This verifies denied
movement/preset/zoom routes and authentication, not active physical transport.

### Actual HTTP motor-mock relink regression (2026-10-05)

Separate `codex/ptz-http-relink-regression` adds an opt-in three-camera fixture
using only loopback media and two local Reolink HTTP mock endpoints. It invokes
real authenticated Crow movement and admin-update routes and actual HTTP clients.
The mock accepts Login and Left/Right/Stop only; it never reaches a physical motor.
Anonymous and bad-CSRF movement must fail before either endpoint receives a command.
Both old components are moving when the view is relinked; the test requires
`A.Left, B.Right, A.Stop, B.Stop` before checking the stored new link. A rejected
Stop must return 409 and preserve the old link; a successful retry commits. A
held HTTP Left command must prevent a concurrent admin relink from changing the
database until the transport acknowledgement is released, after which its
original transport must receive Stop. All waits/releases/fixture shutdowns are
bounded; retained mock movements are stopped before endpoints close.

Actual old native94d7d02b84d4 fails because relink did not stop both old components
(`run-ptz-http-relink-before-20261005`, 15.949 seconds). Packaged nativec6994318de6c
passes the same full fixture in 23.550 seconds, with saved event-order/identity
proof in `artifacts/run-ptz-http-relink-after-20261005/`. This proves actual-handler
HTTP mock transport integration, not physical motor behavior or Baichuan transport.
The package itself is unchanged; only test sources/evidence notes are added.
No private clip was tracked/staged, no user backlog edit or main merge/push/deploy
occurred. Recording-audit handle58671 remains live; 3/128 outputs completed so far.
The matched OFF/ON overhead gate remains open.

### Measured GlobalContext broadcaster-header decoupling (2026-10-05)

On separate `codex/global-context-header-decoupling`, `GlobalContext.h` no longer
imports the full EventBroadcaster/StreamBroadcaster implementations and Crow
through them. The same eager constructor body is defined in `GlobalContext.cpp`;
field declarations and ownership remain unchanged. Consumers invoking broadcaster
methods explicitly include their definitions. The full build caught an existing
transitive logging dependency in the PTZ route, now fixed with explicit `Log.h`.
This is header hygiene/outline construction, not wholesale PIMPL or proof of DLL ABI.

Identical pinned no-PCH TU probes ran three alternating repetitions before/after
the edit, with compiler19.51.36257.0 and the same input TU hash. Context-control
median wall time fell from 7.6377666s to 2.8022738s; preprocessed bytes fell from
10,229,571 to 5,025,223. Unchanged isolated control medians were 2.5492324s and
2.5328541s. Evidence:
`artifacts/message-header-probe-2588956119c94674bc191cdea3e0a74d/summary.json`,
`artifacts/message-header-probe-5600dd28433e46bea162b0658e87412a/summary.json`,
`artifacts/global-context-header-probe-comparison.json` (also records both context
header hashes). A recording audit was running during both probes. These are TU
wall-clock/preprocess measurements, not full-project PCH CPU or runtime speedup.
An initial probe using a nonexistent default triplet failed preflight; the actual
cached `x64-windows-witness-v145-release` triplet was then used consistently.

Final server build native07bf7a9a88f8 succeeded after correcting the logging include;
frontend Sass deprecations/chunk advisories remain. CMake regenerated to add source
and the opt-in test target while reporting the same immutable dependency environment
6b27239cefa9921f3d82643ccc48e50ce241b30f5ea7ce1761240cb59532dc61; no dependency install
or toolchain/global-vcpkg change occurred. `GlobalContextHeaderIsolationTests`
includes the header first, with no server Crow PCH, asserts both broadcasters are
incomplete while shared-pointer member types remain unchanged, and checks eager
service ownership at runtime without starting sockets, threads or cameras. It and
`MessageHeaderIsolationTests` pass. The actual authenticated three-stream replay,
HTTP motor-mock relink and durable-history shutdown/restart fixture passes across
two lifecycles in 54.749s (`artifacts/run-global-context-header-runtime-20261005/`).
Linux compilation and general ABI/production stability remain unverified.

Separate verified package:
`build-vs2026-stable/deploy/context-header-07bf7a9a88f8-web-dbcadd4ab7f8`,
94 files / 178,128,253 bytes; manifest `artifacts/global-context-header-package-verified.json`.
The frozen load package is unchanged. Recording audit58671 remains live; matched
ON has not started. No main merge, remote push, production deployment, private-media
tracking or user-owned backlog edit occurred.

### Rendered dashboard control ownership validation (2026-10-05)

On `codex/dashboard-selection-rendered-validation`, browser-only checks exercised
the current compiled web payload (dbcadd4ab7f8) through the loopback synthetic
`ptzDashboard.mjs --serve --layout` fixture. Its five JPEG cameras and PTZ replies
are fabricated; no request reaches Witness, a physical camera, or production.
In windowed Layout mode, selecting camera 1 displayed its explicit selection
badge and recording controls. Opening PTZ displayed camera 1 as panel owner.
Selecting camera 2 removed camera 1's panel and moved the selection badge;
clicking the overview heading cleared selection. In Full Screen mode the same
badge and named PTZ panel appeared. First Escape dismissed selection and PTZ
while leaving fullscreen active; second Escape returned to windowed Layout.
Entering Arrange with a camera selected cleared its control ownership; Cancel
returned to the original layout without saving fixture changes. The temporary
tab and fixture were closed afterward.

Ignored evidence: `artifacts/fullscreen-selection-20261005.png` and matching
`.dom.txt` show fullscreen selection and the named PTZ panel. This verifies
rendered UI behavior with synthetic JPEG previews, not physical PTZ motor
behavior, decoder capacity, live MSE continuity, or hidden-tab recovery.
No server or web implementation changed, so the verified 07bf7a9a88f8 package
remains the candidate. User-owned BACKLOG.md edits are untouched; no main merge,
push, deployment, or real-media tracking occurred. Matched OFF recording audit
handle58671 was confirmed live, with 6/128 outputs complete and passing; matched
ON and the full overhead/stream-validation gates remain open.

### Retained presentation evidence continuity (2026-10-05)

Separate `codex/presentation-evidence-continuity` hardens the offline replay
summary: adjacent unavailable/conflicting frame snapshots no longer disappear
before progression is calculated, so measurements cannot bridge those gaps.
It also reports advancing/nonadvancing pairs whose two endpoints are explicitly
presentation-visible separately from hidden or unknown visibility. This is
sampled callback evidence, not uninterrupted video or browser occlusion proof.
All 17 summary regression tests pass, including missing/zero/nonfinite callback
data, conflicting duplicate snapshots, and mixed visibility pair endpoints.

Re-analysis of the frozen OFF phase is saved in ignored
`artifacts/matched-cached-health-off-v2-continuity-summary.json`. One eligible
browser session retained 119 snapshots over 1773.428s; largest receipt gap was
15.063s, with measurement start/end offsets 14.279/13.231s. Every one of its 16
players has 118 visible advancing pairs, zero unmeasurable/nonadvancing pairs,
zero sampled counter resets/restarts, and final readyState 4. Final latencies
range 1226-1631ms. These four profiles repeated four times are not 16 independent
physical cameras, and the retained snapshots cannot exclude between-sample
stalls. The native package and frozen phase inputs are unchanged; no rebuild is
required for this offline analysis-only change.

Recording-audit handle58671 remains live, with ffprobe2484 consuming CPU during
the check. Its first six of 128 outputs have passed; remaining recordings,
matched ON overhead and true hidden/resume/independent-profile validation remain
outstanding. No main merge, push, deployment, real-media tracking or user backlog
edit occurred.

### Client telemetry final wire-budget fallback (2026-10-05)

Separate `codex/client-health-post-budget` reproduces a client compaction gap:
64 bounded player records with worst-case UTF-8 strings and finite numeric
scalars can still exceed the 60KiB target after event/drift details are stripped.
The new regression failed on the unchanged reporter, then passed with the final
fallback. It retains a stable prefix of player records fitting the actual
UTF-8 JSON budget, sets `playersTruncated`, and leaves the input snapshot intact.
The byte accounting is a linear pass over records, including comma overhead;
ordinary rich 16-player reports retain their existing causal evidence. This is
explicit partial coverage, not silent loss of the entire over-budget POST.

Reporter, hidden recovery, presentation, visibility and eviction suites pass
31 tests with one optional offline fixture skipped. Type checking and the
pinned server/frontend build pass; Sass/Bootstrap deprecations and chunk-size
advisories remain. A sandbox Node worker launch failed with EPERM, not a test
failure; in-process tests ran successfully. The actual authenticated three-stream
loopback server/history shutdown/restart fixture passes two lifecycles in
39.427s (`artifacts/run-health-budget-runtime-20261005/`). That fixture exercises
the packaged native lifecycle and history routes; it does not render the new
64-player browser report, prove true browser hide/resume, or establish stability.

Separate verified package:
`build-vs2026-stable/deploy/health-budget-07bf7a9a88f8-web-ae5c875b236a`,
94 files / 178,128,485 bytes; `artifacts/health-budget-package-verified.json`.
Native identity stays 07bf7a9a88f8 because only web/test code changed; the web
identity is ae5c875b236a. The frozen matched-load package remains unchanged.
Audit58671 remains live and has completed nine of 128 recording inspections;
matched ON still awaits its terminal result. No dependency/toolchain change,
main merge, remote push, production deployment, real-media tracking or
user-owned backlog edit occurred.

### Genuine hidden-tab validation tooling limit (2026-10-05)

On `codex/browser-visibility-validation`, a read-only browser-native Page
Visibility probe used the fixed loopback synthetic recovery page, without
starting playback or overriding browser state. Only the in-app browser and MCP
Apps surface were available. The recovery tab reported document.hidden=false /
visibilityState=visible before and after another temporary tab was opened.
Hiding the browser panel is a presentation capability, not proof of a hidden
document. No genuine hidden transition was observed, so this does not validate
hidden/resume behavior or independent browser profiles. Do not replace the open
gate with the already-passing simulated visibility/watchdog regression tests.
The computer-use check ended at this tooling limitation; temporary tabs and
the synthetic fixture were closed. Ignored evidence:
`artifacts/iab-hidden-visibility-probe-20261005.json`.

Audit handle58671 was confirmed live again and its progress reached 10/128
recordings. The matched ON phase remains unstarted while recording inspection
is active. This evidence-only branch changes no runtime/package, main, remote,
production configuration, physical camera, private-media tracking or user backlog.

### Reproducible matched replay numeric comparison (2026-10-05)

Separate `codex/matched-replay-comparison` adds
`Tools/Tests/compare_replay_phases.py OFF_SUMMARY ON_SUMMARY OFF_RECORDINGS ON_RECORDINGS`.
Its summaries now retain actual per-sample observer modes and main-stream codec,
resolution/audio profiles. Missing/mixed mode evidence or changing profiles stays
unavailable rather than being inferred from requested environment flags.
The comparison requires identical native/web identities, observed profiles and
CPU-inference/clip+DVR workload, sixteen uniquely identified H.264/HEVC replay
streams, each phase at least 1800s with sampling gaps no larger than 10s, browser
attachment before the phase, complete ordinary-summary HTTP timings, exact
per-camera filter task distributions, and completed passing recording inspection
covering clips and DVR for every camera. Missing qualifications return incomplete.

It reports CPU P50 and P95 changes separately in host percentage points (both must
be below one), exact per-camera filter P95 increments (each below 5%), and each
phase's ordinary-summary HTTP P95 (below 100ms). CLI exit is nonzero for failed or
incomplete inputs. This is deliberately a replay numeric-threshold result, not
a claim of full goal completion, production stability, physical-camera diversity,
continuous playback, hidden/resume or independent-profile browser validation.
Those separate requirements remain open even if numericThresholdsMet is reported.

Three comparison and eighteen summary regression tests pass. Re-analysis of the
actual frozen OFF phase reports all five observers disabled consistently across
the sampled phase and sixteen stable main profiles. Ignored evidence:
`artifacts/matched-cached-health-off-v2-qualified-summary.json` and
`artifacts/matched-cached-health-comparison-pending-on.json`. Supplying no ON phase
and the still-running OFF inspection correctly produces incomplete, not a pass.
The audit58671 handle remains live; ON has not started. Runtime/package/source
dependencies, main, remote, production, private-media tracking and user backlog
are unchanged; no build is necessary for this offline test/analysis change.

### Bounded browser-history SQLite contention recovery (2026-10-05)

Separate `codex/client-health-storage-contention` reproduces a storage-worker
liveness gap using a newly-created private telemetry DB: an external BEGIN
IMMEDIATE held for 650ms made the old worker exit permanently after its 200ms
busy timeout. Releasing the lock never made the accepted sample durable; the
opt-in regression failed with history worker timed out. No real database,
application sessions, recording files, physical camera or production setting
were used.

SQLite failures now retain their primary result code internally. Startup/load
and batch transactions retry only SQLITE_BUSY/SQLITE_LOCKED, at most eight times
with a 100ms interruptible worker wait and the existing 200ms SQLite busy timeout.
Each unsuccessful transaction rolls back. The same accepted batch and sequence
IDs are retried, without re-appending samples or advancing the durable watermark
until commit. Storage availability is false during retries, failures remain
cumulative evidence, and availability returns after successful storage. HTTP
handlers never perform the wait. Shutdown interrupts backoff; other storage
errors and exhausted contention still terminate with unavailable storage. This
is bounded brief-contention recovery, not indefinite reopening/recovery from a
prolonged lock, permission failure, full disk or corrupt database.

`ClientHealthHistoryTests <NEW_PRIVATE_DB> --temporary-contention` passes: write
and startup locks recover, accepted rows persist without duplication across
restart, shutdown during retry completes within one second, and a lock exceeding
the budget leaves durability unchanged and rejects new submissions. The full
expanded contention suite passes twice on distinct fixtures. An intermediate
run raced the fixture's zero-timeout COMMIT and failed at unlock; the external
fixture connection now has a bounded 1000ms busy timeout and repeated tests pass.
The full persistence/limits/concurrent-reader suite also passes, with near-limit
export 16ms / 9,393,620 serialized bytes. Its initial run timed out while native
compilation was active; a fresh standalone rerun passed. The cause of that first
timeout is unproven, not dismissed as a confirmed compiler effect.

Pinned native/frontend build passes with cached dependencies; frontend Sass and
chunk advisories remain. Actual authenticated zero-camera packaged-server
history/shutdown/restart passes two lifecycles in 18.599s
(`artifacts/run-health-contention-runtime-20261005/`). It is not live-camera,
hidden-tab, production-stability or full-load validation.
Separate verified package:
`build-vs2026-stable/deploy/health-contention-bc40858364cf-web-a4264754aefb`,
94 files / 178,163,813 bytes; manifest `artifacts/health-contention-package-verified.json`.
The older frozen load candidate is unchanged; its audit58671 remains live with
13/128 inspections complete. Matched ON and validation of the latest candidate
remain open. No main merge, remote push, production deployment, dependency or
toolchain mutation, private-media tracking or user-owned backlog edit occurred.

### Latest mixed-codec candidate: shutdown validation failure (2026-10-05)

The packaged native `bc40858364cf` / web `a4264754aefb` candidate failed the
four-profile, 90-second CPU-inference/manual-recording/DVR replay with all five
observer modes enabled. The qualified inputs include H.264, 1080p HEVC, 2160p
HEVC and H.264/AAC; the inference workload was not reduced to bypass validation.
The server did not finish stopping within the existing 30-second limit. This
is a shutdown timeout, not a crash, and neither recording inspection nor the
second lifecycle was reached. The earlier zero-camera pass does not override
this camera-loaded failure. No candidate stability claim is supported.

Private evidence remains ignored under
`artifacts/run-health-contention-mixed-codecs-20261005-v2/`: the replay and raw
processing samples, failure logs, and owned-child shutdown dump/stack log
`owned-shutdown-4193f014dfa1f858`. Matching package symbols show the main thread
waiting on `ServerControlMutex` in `ServiceMain`, the history worker in its
normal condition-variable wait, and the reprocessing worker inside its
30-second idle sleep. The control-handler thread could not be inspected
(dump reports thread access failure), so its precise blocking operation and
the cause of the timeout remain unproven. An uninterruptible idle sleep is a
lead, not proof that this was the only cause or introduced by the storage fix.

An identical four-profile comparison against immutable native `2213c5ef8142` /
web `b301018e1023` is running separately under
`artifacts/run-health-contention-baseline-mixed-codecs-20261005/`. Its first
shutdown completed and recording inspection began; this partial result does
not prove the older candidate immune to intermittent shutdown delays. The
comparison is not the matched 30-minute OFF/ON gate. The large OFF audit is
still running separately; ON must not begin while either audit is active.

The baseline invocation subsequently completed all eight first-lifecycle
recording inspections (strict decode and packet/frame timestamp checks). Its
second lifecycle failed before starting a server: replay sender creation tried
to inherit an invalid standard-input handle after the fixture's console
detach (`WinError 6`). This is a harness failure, not a baseline server crash
or a two-lifecycle pass. Replay senders, server children and dump symbolization
now use explicit null input instead of inheriting the detached console handle.

Shutdown observation now captures an exclusive owned-child dump after ten
seconds, while still attached to the child's console. It then waits only the
remaining original 30-second budget. Symbolization happens after shutdown or
owned-child cleanup, not inside the deadline. Fast exits produce no dump;
observation failure does not replace the server result. Five offline timing/
sender-input regressions pass, including time spent observing counting toward
the deadline and timeout propagation without another 30-second allowance.
A fresh latest-package run with the unchanged qualified workload under
`artifacts/run-health-contention-mixed-codecs-early-stacks-20261005/` reproduced
the shutdown timeout (139.093s invocation, first lifecycle only). Its exclusive
ten-second dump `owned-slow-shutdown-7607f6aab438dfda` was manually symbolized
against the immutable package into `.early-stacks.log`: the main thread again
waits on the control mutex and the reprocessing worker is sleeping. Windows
again denied access to the console-handler thread, even before console detach.
The normal minidump lacks the worker object's heap memory, so its shutdown flag
could not be read. These observations do not prove which join/operation the
handler is executing or that idle sleep alone caused the failure. The final
timeout dump `owned-shutdown-4bd8e920cf0020d7` remains private and ignored too.
Targeted shutdown-stage tracing or richer test-owned evidence is still needed.
These changes affect diagnostic tooling, not the packaged server, and do not
fix or dismiss the earlier shutdown failure. No new deployable build is claimed.

The same replay reproduced the timeout again in 135.650s under
`artifacts/run-health-contention-mixed-codecs-referenced-memory-20261005/`.
Test-owned dump capture now optionally includes indirectly referenced memory
(not full process memory); its default remains a normal thread-info dump.
This option is restricted by convention to private synthetic fixtures and may
contain heap data/fixture credentials: never publish these files. The owned
Python-child dump/exclusive-output smoke passes for both modes.

The richer ten-second and timeout snapshots contain the reprocessing worker
object. Matching symbols locate `WorkerBase::Shutdown` at offset 0x48 and
`Complete` at 0x49. At both snapshots the reprocessing worker reads 01/00 while
its stack remains inside the 30-second idle sleep; the audio worker reads 00/00.
This proves an accepted reprocessing stop cannot interrupt its idle sleep.
The shutdown implementation joins reprocessing before requesting audio stop,
consistent with the audio state. It does not prove every other teardown stage
is healthy. Stack and worker-state logs remain ignored alongside the dumps.

### Interruptible worker idle cancellation (in progress, 2026-10-05)

Branch `codex/interruptible-worker-idle-shutdown` adds a predicate-based idle
condition variable to server-only `WorkerBase`. Stop publication and wait entry
share a small mutex to prevent missed wakeups; stop wakes all idle waiters before
the existing first-request message-bus notification. Normal waits retain their
original duration. Reprocessing (including AI reservation backoff), audio,
watchdog and timer waits use this helper. It does not interrupt inference,
filesystem, database, camera reads or other work already in progress, and does
not claim to fix every potential shutdown blocker. No DLL-exported WorkerBase
usage was found in WitnessCamera or WitnessCommon; full server consumers must
be rebuilt because the private base layout changes.

The rebuilt native `WorkerShutdownDelayTests` executable passes ordinary timeout,
pre-existing/repeated cancellation, multiple waiters and 100 request-before/
during-wait races. It passes again alongside existing HealthSnapshotCacheTests
and PtzMovementLeaseTests. Full pinned native/frontend build passes (session37708),
using unchanged immutable dependency fingerprint
`6b27239cefa9921f3d82643ccc48e50ce241b30f5ea7ce1761240cb59532dc61`.
The sandbox invocation built the native test then stopped before server build;
the scoped elevated invocation completed rebuilding server consumers. Existing
frontend Sass deprecations/chunk-size advisories remain; this is not a warning-
free build claim. Separate package
`build-vs2026-stable/deploy/worker-idle-38f1f29951b5-web-75e216083a96` is verified
against build output (94 files / 178,199,141 bytes; manifest
`artifacts/worker-idle-package-verified.json`). This verifies byte/inventory/
native/web identity coherence, not broad ABI or production stability.

Qualified four-profile replay against that immutable package is running under
`artifacts/run-worker-idle-mixed-codecs-20261005/` (session69740), retaining the
90-second CPU inference/manual recording/DVR workload, five observers and
original 30-second shutdown deadline. Its first shutdown completed and two of
eight first-lifecycle recording inspections have passed; the remaining recording
checks and second lifecycle are pending. This partial result is not a final
camera-loaded integration pass or the matched 30-minute endurance gate. The
frozen OFF audit remains live and has advanced to 14/128 inspected outputs;
ON remains deferred until background audits end. No main merge, remote push,
production deployment, dependency mutation or private-media tracking occurred.

### Completed mixed-codec shutdown validation (2026-10-05)

Immutable native `38f1f29951b5` / web `75e216083a96` completed both qualified
four-profile lifecycles in 681.976s; all eight outputs in each recording audit
passed. The join-hardened native `90c2fae6e2a5` / web `743329f05c56` subsequently
completed the same two-lifecycle test in 692.434s, with all sixteen recording
inspections passing (`artifacts/run-worker-join-mixed-codecs-20261005/`). These
are terminal test results, not inferred from progress files. Both tests retain
90-second CPU inference/manual recording/DVR phases, five observer modes and
the original 30-second shutdown deadline. They check authenticated history,
processing evidence, qualified codecs, strict decode and packet/frame timestamp
inspection. Neither attaches a browser or uses physical cameras, and the
invocation duration includes slow offline recording audits: it is not a CPU/
latency benchmark or the matched 30-minute load gate. The new package therefore
has camera-loaded lifecycle evidence, not a production-stability claim.

### Rendered automatic-slot and rectangle checks (2026-10-05)

Branch `codex/dashboard-auto-slot-rendered-validation` records CUA validation
of the compiled web `743329f05c56` dashboard using only the existing synthetic
PTZ/SVG/JPEG fixture. Eight offline dashboard-layout tests pass. In Layout mode,
fixed camera 10 remains visible while active cameras 15/14 occupy Auto 1/2 in
priority order, with all five group camera cards still mounted. UI Arrange/Save
successfully changes the activity rectangle to x9/y7/width3/height5, reflected
as left75%/top58.3333%/width25%/height41.6667% in rendered DOM.

The UI saves a 20-second automatic-slot hold. Camera 15 remains Auto 1 at the
observation about 7.7 seconds after its stop event; a later observation about
57.9 seconds afterward shows active camera 14 as Auto 1 and fallback camera 13
as Auto 2. This checks retention and subsequent release, not exact threshold
timing. The initial six-second observation missed the hold window and is not
treated as a proven failure or pass. Five mounted cards do not prove continuous
video decoding, hardware decoder capacity, keyframe readiness or promotion
latency. Per-group persistence across independent browsers remains unverified.

The first fixture was restarted with interactive input for motion injection;
both initial fixtures ended through their existing bounded timers. A screenshot
captured around fixture expiry shows a connection overlay, not a native server
fault. A fresh synthetic fixture provides clean visual evidence in ignored
`artifacts/dashboard-auto-slot-priority-20261005.png`; the screenshot depicts
priority slots with the default rectangular panel, while the edited-rectangle
and hold observations are recorded separately in ignored
`artifacts/dashboard-auto-slot-observations-20261005.json`. The final tab/server
were explicitly closed (owned PTY interrupt). No real feeds, camera movement,
production authentication changes or private media were used in this UI check.

One concrete responsive defect remains: at viewport width1280, playback and
Activity toolbar buttons extend to x1330.45/x1401.30. This is visible in the clean
screenshot and confirmed from DOM bounds; it is not a claim that every keyboard
navigation path is unavailable. Responsive toolbar handling is the next UI fix.

### Explicit worker joins before normal teardown (2026-10-05)

Branch `codex/worker-join-before-teardown` closes a normal-shutdown ordering
gap found while reviewing idle cancellation: thumbnail, watchdog and timer
workers received stop requests, but only joined from their base destructors.
In particular, watchdog targets/timer callbacks are derived members and their
destruction precedes the base destructor. `RequestShutdown` now explicitly joins
all three after image queues/workers have been stopped and joined, before
`Shutdown` stops shared broadcasters and destroys worker objects. It does not
hold `Context->Mutex` while joining. This is normal-path ordering hardening,
not proof that arbitrary blocking callbacks or partial initialization are safe.

The production-source ordering guard fails for all three missing joins against
the earlier committed source and passes for the new source; it ignores comments
and quoted strings and bounds evidence to the RequestShutdown function body.
This is a source-order regression, not a dynamic held-callback lifetime test.
Pinned native/frontend build and WorkerShutdownDelayTests pass. Separate package
`build-vs2026-stable/deploy/worker-join-90c2fae6e2a5-web-743329f05c56` is byte/
inventory/identity verified (94 files / 178,199,141 bytes;
`artifacts/worker-join-package-verified.json`). Actual authenticated zero-camera
health-history shutdown/restart passes two lifecycles in 14.418s under
`artifacts/run-worker-join-runtime-20261005/`. That does not validate live streams,
physical PTZ, delayed callbacks, full load, Linux or production stability.

The immutable preceding candidate `38f1f29951b5` completed both qualified
camera-loaded shutdowns without timeout. Its first recording audit passed all
eight outputs; four of eight second-lifecycle outputs have passed, with the
final result still pending (session69740). The join-hardened package still needs
its own qualified camera-loaded replay after that audit completes. Frozen OFF
inspection session58671 remains live and has advanced to 22/128 outputs. No
matched ON phase is started during these audits. No main merge/push, production
deployment, dependency mutation, media tracking or user-owned backlog edit.

### Responsive dashboard toolbar (2026-10-05)

Branch `codex/dashboard-toolbar-responsive` opts the dashboard into an
auto-height, wrapping header without changing other pages' fixed-height
headers. Its toolbar wraps controls instead of clipping playback and Activity
past the viewport. Camera group buttons are independently wrapping, with long
names allowed to break. Existing mobile zoom hiding remains unchanged.

Vue type checking and all eight dashboard layout tests pass. The production
frontend bundle builds with existing Sass deprecation and chunk-size warnings.
Computer-use validation used only the synthetic five-camera loopback fixture:
all displayed controls were within the viewport at widths 1280, 1024 and 390;
the Activity toggle worked at 1024. Ignored screenshots are
`artifacts/dashboard-toolbar-1280-20261005.png` and
`artifacts/dashboard-toolbar-mobile-20261005.png`. This fixture has no camera
groups, so many/long group names still need rendered validation. Browser
viewport override was reset and the owned fixture/tab were closed.

Separate package
`build-vs2026-stable/deploy/toolbar-responsive-90c2fae6e2a5-web-2b399a208d85`
contains the unchanged tested native build and new web bundle; byte/inventory/
identity verification passes (94 files / 178,199,729 bytes), recorded in
`artifacts/toolbar-responsive-package-verified.json`. The prior runtime Web
directory was retained as `Web-toolbar-prior-743329f05c56`, not deleted. No
new native runtime, live-stream, Linux or production-stability claim follows
from these frontend checks. Frozen OFF recording audit session58671 remains
live; matched ON has not started. No push or production deployment.

### Long camera-group toolbar validation (2026-10-05)

Branch `codex/dashboard-toolbar-group-validation` adds bounded synthetic
`--many-groups` and `--few-long-groups` fixtures. Rendered testing caught a real
mobile overflow in the preceding wrapping-button rule: a later equal-specificity
nowrap declaration overrode it. The corrected rule is more specific and caps
individual button width. More than three camera groups now use a compact
accessible select rather than making the toolbar several rows taller. A long
selected group title also wraps inside the dashboard heading.

At 390px, the six-group picker selected the longest name successfully with no
clipped toolbar controls (215px header versus the prior six-button 430px header).
The final two-group button case also selected the longest name successfully:
all controls fit, and the selected heading stayed within the viewport. Its
screenshot is `artifacts/dashboard-toolbar-small-long-groups-20261005.png`.
The DVR rendered header remains 52px with wrapping opt-in absent. Viewport
overrides were reset; owned tabs and synthetic fixtures were closed. Fixture
API consistency tests (empty/six/two groups), eight layout tests and Vue type
checking pass; production frontend build retains existing Sass/chunk warnings.

Package `build-vs2026-stable/deploy/toolbar-groups-90c2fae6e2a5-web-a2c81ac6bfeb`
is separately byte/inventory/identity verified by
`artifacts/toolbar-groups-package-verified.json`. Native binaries are unchanged;
this is frontend validation, not new stream endurance or production stability
evidence. Prior runtime Web is recoverable as `Web-toolbar-prior-2b399a208d85`.
No real camera media is part of these fixtures, and no push/deployment occurred.

### Dependency inventory linked-root guard (2026-10-05)

Branch `codex/dependency-inventory-root-guard` fixes an evidence gap in the
read-only dependency verifier: it previously resolved the supplied installation
root before checking links, silently accepting a root symlink/junction even
though linked descendants are rejected. Both API and capture CLI now validate
the supplied root before resolution. Regression coverage includes symlink,
junction and CLI entry paths; platform link-query mocks avoid requiring Windows
symlink privileges. This is not evidence against arbitrary ancestor-link races.

The root-link regression fails against the earlier implementation and passes
afterward. All seven inventory tests pass. All 14 dependency-policy tests pass
when the real read-only checkout/tool validation is enabled with
`WITNESS_TEST_VCPKG_ROOT=X:/Programming/vcpkg`; the earlier general run had that
one optional test skipped, then it was explicitly run and passed. Tests required
access to private temporary directories; the initial sandbox run produced ACL
errors, not application regressions.

Fresh capture `artifacts/dependency-root-guard-current-20261005.json` inventories
2,673 installed files / 759,367,637 bytes. Comparison with
`artifacts/dependency-stable-before-20261004.json` reports zero added, removed or
changed files. No install, bootstrap, toolchain mutation or rebuild occurred.
This is tooling-only: the verified deployable native90c/web-a2c81 package from
the preceding section remains unchanged. Frozen OFF audit58671 is confirmed
live, with 56/128 outputs inspected successfully; overall inspection remains
incomplete and matched ON has not started.

### Actual full-server multi-client history authorization (2026-10-05)

Branch `codex/full-server-multi-client-history` adds opt-in
`WITNESS_TEST_MULTI_CLIENT_HISTORY=1` verification to the isolated native-server
lifecycle harness. It requires zero cameras and at least two real lifecycles,
keeping these authorization checks outside measured replay phases. All users,
session tokens, database, cache and server binding are private fixture-owned;
no live sessions or production credentials are read or changed.

Two ordinary users concurrently submit identical browser/tab labels with forged
owner fields. The administrator export retains two distinct authenticated
owners, strips the forged name, and reports their hidden-state samples durably.
Ordinary users cannot read the all-client export and cross-user CSRF fails.
Disabling a previously accepted user immediately denies its next write. A
deleted session cannot write, while its previously accepted history remains.
Removing admin privilege immediately denies the separate exporter's next read;
restoring that privilege permits reads again, and deleting its session denies
reads again. Both retained owners and revocations survive actual server restart.

The existing immutable native90c/web-a2c81 package passed the initial two-user
test in 14.096s and the expanded fresh-authorization test in 13.976s. Per-lifecycle
reports are under `artifacts/run-full-server-multi-client-fresh-auth-20261005/`.
No application-code fix or rebuild was needed. This proves production-handler
HTTP/session/storage behavior, not independent browser-profile collection or
real hidden/resume playback. Frozen OFF audit58671 is still live and now has
58/128 outputs inspected; the overall stream/performance gates remain open.
No main merge, remote push, deployment, media tracking or backlog edit.

### Real held timer callback lifetime regression (2026-10-05)

Branch `codex/worker-held-callback-regression` strengthens the earlier normal
shutdown ordering evidence. The Windows WorkerShutdownDelayTests target now
links the real WorkerBase and TimerWorker implementations, not a fake worker
loop. A real timer callback signals entry, holds shared callback state, and
waits for release while shutdown is requested. A separate join stays blocked
for the observed 100ms hold, completes within two seconds after release, and
timer teardown then releases the last callback-state owner. A five-second
callback failsafe bounds cleanup if a preceding assertion fails. This is not
proof that arbitrary production callbacks are interruptible or bounded.

Real held-callback and existing idle cancellation tests pass, as does the
production RequestShutdown source-order guard. Their combined coverage verifies
native Join behavior and explicit production join placement; the test does not
invoke WitnessServer::RequestShutdown with an injected timer. Linux retains
the existing idle tests, with the Windows-only dynamic callback case explicitly
reported as untested there. WorkerBase.cpp now includes its own header rather
than WatchdogWorker/Messages; TimerWorker.cpp also drops unnecessary Messages.
The isolated no-server-PCH target builds these real implementations without the
broader message/media dependencies.

Pinned incremental full native/frontend build passes using immutable dependency
fingerprint `6b27239cefa9921f3d82643ccc48e50ce241b30f5ea7ce1761240cb59532dc61`.
The first sandbox full-build attempt exited in MSBuild without diagnostics;
the scoped build retry completed successfully. Existing Sass deprecations and
frontend chunk advisories remain; this is not a clean full warning/ABI audit.
Package `build-vs2026-stable/deploy/worker-callback-c96b97f1c1b9-web-30cc8de26b50`
passes byte/inventory/identity verification (94 files / 178,200,382 bytes;
`artifacts/worker-callback-package-verified.json`). The packaged native build
passes the actual authenticated zero-camera two-lifecycle multi-client history
fixture in 16.106s, under `artifacts/run-worker-callback-multi-client-20261005/`.
Live-stream endurance, physical PTZ and Linux compilation remain open. Frozen
OFF audit58671 is still live: 63/128 outputs inspected, none failed so far;
matched ON is not started. No push, production deployment or backlog edit.

### Fresh native warning and DLL audit (2026-10-05)

Branch `codex/clean-native-warning-audit` records an actual `--clean-first`
RelWithDebInfo rebuild of WitnessServer, WitnessCommon, WitnessCamera,
BaichuanLifetimeTests, LiveStreamOwnershipTests and WorkerShutdownDelayTests with
the pinned compiler and `/m:2 /nr:false`. Log:
`artifacts/native-clean-warning-audit-20261005.log`. All targets finish with
exit zero; the complete log contains zero `warning C...` or `warning LNK...`
lines. Camera/server targets retain explicit `/W3`, and the inspected project
files/export headers do not disable C4251/C4275. Existing frontend Sass
deprecations and the large-chunk advisory remain. This is the current Windows
configuration, not every platform/configuration or general ABI compatibility.

The freshly linked DLL passes 20 synthetic loopback Baichuan snapshot/stalled
TCP shutdown cycles. The live ownership consumer passes eight H264 video-only
reconnect/destruction cycles using the bounded offline H264 fixture. The real
held timer callback and idle shutdown tests pass. The short ADTS/AAC fixture
fails the live ownership test's `retained segment ownership` assertion; the
same newly compiled consumer also fails with the previous immutable native90c
DLLs in `artifacts/live-ownership-control-90c2fae6e2a5/`. This is an open AAC
validation issue, not evidence of a regression caused by the latest includes.
The test feeds exactly 100 packets per generation and only checks that the
backlog is nonempty, while GetSegments can include an unfinished entry without
Data. GOP/audio packet-count dependence is the next investigation, not yet a
confirmed root cause. Do not count the AAC ownership gate as passed.

Fresh full dependency capture `artifacts/dependency-clean-native-after-20261005.json`
matches the pre-rebuild inventory exactly: 2,673 files / 759,367,637 bytes,
zero additions, removals or content changes. No installed dependencies were
cleaned/rebuilt. The available second compiler is a different 14.44 family,
not a same-family servicing A/B control; compiler-servicing ABI proof remains
unperformed.

Clean-rebuilt package
`build-vs2026-stable/deploy/native-clean-c96b97f1c1b9-web-2c02a751180d`
passes byte/inventory/identity verification (94 files / 174,735,166 bytes;
`artifacts/native-clean-package-verified.json`) and actual zero-camera
authenticated two-lifecycle multi-client history checks in 15.296s, under
`artifacts/run-native-clean-runtime-20261005/`. Native source identity is
unchanged; a clean link changes binary/symbol bytes, so prior package sizes are
not expected to match. Frozen OFF audit58671 is confirmed live at 72/128,
with zero inspected failures; overall inspection and matched ON remain open.
No push, main merge, production deployment, media tracking or backlog edit.

### Completed-media ownership and pre-header EOF teardown (2026-10-05)

Branch `codex/live-ownership-completed-segments` resolves the AAC ownership
test's fixed-packet-count assumption. With the previous 100-packet window,
the AAC fixture had one unfinished backlog entry and zero completed segments;
the updated consumer waits for actual ready, nonempty media, with a 5-second /
5,000-packet bound and the original minimum 100 packets. AAC requires 137
packets for its first completed segment. Full segments, their published
partials, callback buffers and the retained init are hashed across reconnect
and destruction, rather than accepting an unfinished backlog entry.

A synthetic 0.5-second copy of that fixture exposed a separate native crash:
EOF before the first usable keyframe leaves an allocated MP4 context/custom IO
without a written header. Shutdown previously flushed it unconditionally;
FFmpeg called a null interleave function. The debugger stack is retained only
locally in `artifacts/live-ownership-incomplete-eof-cdb-20261005.log`.
Shutdown now flushes only after a successful header and clears the header flag
when freeing the context. This does not change normal established-stream muxing.

The new `preheader` consumer mode reaches EOF and verifies no init was written,
then exercises explicit repeated shutdown, ordinary destruction, exception
unwinding, and reconnect teardown. Against the previous native90c DLL control,
the same consumer hits an access violation in mode 0 after 35 packets, through
`avformat_62!interleaved_write_packet` and `LiveOutputStream::Shutdown`.
Debugger exit zero means the debugger quit, NOT a passing application test.
Log: `artifacts/live-preheader-eof-old-dll-cdb-20261005.log`.
Against native `cbbf475693c1`, all four paths pass. The original short-fixture
qualification also now exits normally with code 1 instead of crashing; only
that exact exit is accepted as the expected negative result.

The freshly built DLL passes eight completed-media ownership/reconnect cycles
each with H264, H264+AAC and HEVC 1080p20 fixtures. Logs are
`artifacts/live-preheader-valid-{h264,aac,hevc}-20261005.log` and
`artifacts/live-preheader-eof-regression-20261005.log`. The HEVC fixture has
protected local file access and required a scoped elevated test. The pinned
native/frontend build passes with no native compiler/linker warning lines;
existing Sass/chunk advisories remain.

Separate package
`build-vs2026-stable/deploy/preheader-eof-cbbf475693c1-web-0bc79b105145`
passes byte/identity verification (94 files / 177,889,086 bytes;
`artifacts/preheader-eof-package-verified-20261005.json`). Actual authenticated
zero-camera two-lifecycle multi-client history checks pass in 18.481 seconds,
under `artifacts/run-preheader-eof-runtime-20261005/`. These are offline codec
and isolated runtime checks, not production endurance or proof that this
teardown bug explains an observed camera stall.

Frozen OFF audit58671 remains live: 88/128 full recordings inspected, with zero
inspected failures; its immutable candidate predates this fix. Matched ON,
latest-candidate endurance, real hidden/resume and independent browser-profile
validation remain open. No private fixture/capture is tracked, and no push,
main merge, production deployment or user backlog edit was performed.

### Current-DLL cadence harness lifetime and generation validation (2026-10-05)

Branch `codex/cadence-harness-teardown-validation` changes test consumers only.
LiveCadencePauseTests and LiveTimestampRecoveryTests now declare every captured
output/counter before the live stream, so stream teardown cannot outlive that
callback state on normal or early-error exits. The pause harness's normal
callback clearing alone did not cover its exception paths. This is a lifetime
ordering defect found by inspection, not a claimed reproduced production crash.
The pause harness also emits numeric evidence for all six scenarios.

Pinned builds of both real-Camera-DLL consumers and TimestampRegressionGuardTests
pass. Both qualified offline fixtures are 256x144, 20fps, 150 seconds, no B-frames,
one H264 and one HEVC stream. Native `cbbf475693c1` passes both codecs through
the synthetic 15-second pause/burst, bounded learned delivery, intact-source-clock
negative control, unstable-delivery rejection and cooldown/keyframe requalification.
The replay epoch is virtual in the pause test, NOT a real 140-second live test;
the separate requalification test uses actual 50ms paced input for 380 frames.

For each codec the control accepts 2,799 packets with zero drops; pause accepts
2,798 with zero timestamp drops, one reset, a 15,050ms observed gap and no retry
budget consumed. The 56ms bounded-delivery case learns 56ms, has zero drops and
0.333333ms final phase error. An intact source clock stays unnormalized; the
unstable-delivery negative control records rejection and 546 drops. The exported
TimestampNormalizationActive field describes the last completed segment, while
rejection is retained evidence, so both can be true in the negative control;
these reports must not be interpreted as a new current-state normalization API.

All writers terminate before authoritative strict checks. The twelve original
outputs (eight pause/control/bounded-rate generations and four cooldown/retry
generations) pass full strict decode, with zero decoder diagnostics and no missing,
duplicate or regressing packet/frame timestamps. Report:
`artifacts/cadence-current-cbb-terminal-full-decode-20261005.json`.
The rebuilt numeric reporter's eight fresh output generations pass again:
`artifacts/cadence-current-cbb-numeric-terminal-decode-20261005.json`;
per-scenario logs are `artifacts/cadence-current-cbb-numeric-{h264,hevc}-20261005.log`.
An earlier broad-glob inspection included a still-running retry writer and is
invalid qualification evidence (`artifacts/cadence-current-cbb-full-decode-20261005.json`);
its transient failure disappeared after that writer closed normally. Saved
partial generations omit buffered tails; decode cleanliness does not prove every
accepted access unit was published or continuous browser presentation.

There is no runtime/frontend source change or new deployable binary identity.
The preceding verified preheader-eof package remains the deployable candidate.
Frozen OFF audit58671 is still live with 90/128 inspected and zero inspected
failures. Overall inspection, matched ON, latest-candidate endurance and physical
camera/browser hidden-resume validation are still open. No private recording,
user backlog edit, remote push or production configuration change is committed.

### MSE first-presentation watchdog gap (2026-10-05)

Branch `codex/mse-first-presentation-recovery` adds a red/green regression through
the actual production composable: fragments and the media clock keep advancing,
frame callback observation is supported/armed, but no first video frame is ever
reported. Before the change, presentation.mediaTime remains undefined forever,
the watchdog records no presentation stall and restartCount stays zero. Existing
clock/transport/drift checks do not cover this apparently healthy-clock case.
This is a reproduced simulated control-path gap, not proof of the physical Tapo
camera's current failure mode.

Presentation tracking now explicitly records that callback observation was
installed. Only then can a missing first frame become evidence: the eligible
visible player's clock must advance more than one second from a fresh eligible
baseline, followed by the existing >3-second runnable presentation timeout.
Unavailable callback support still remains unknown and retains existing recovery.
Hidden/offscreen/covered/paused/seeking/render-suppressed intervals do not spend
the first-frame timeout; discontinuity, ineligibility and clock rewind reset
its baseline. Suspension still contributes only the existing 250ms runnable
tick allowance. Unknown first presentation now also requires viewport hit-test
visibility, avoiding false recovery beneath an external covering dialog.

The previously failing composable regression now passes. Recovery, presentation,
visibility, eviction, audio-play guard, compact health reporting, drift and health
display suites pass 45 tests with one optional offline export skipped. Type checking
and the pinned server/frontend build pass. Existing Sass/chunk advisories remain;
no dependency/toolchain change was requested.

Computer-use skill/browser inspection ran the freshly esbuild-bundled actual
composable against the fixed hashed synthetic H264/AAC 35-second browser fixture.
137 pre-end snapshots through currentTime 33.971124 show no restart/stall/error;
the last pre-end drift sample has 713 presented callbacks, readyState4,
presentationVisible=true, presentationStallMs=0, and empty append queue.
The finite fixture then exhausts, and the existing frozen-clock watchdog performs
one expected tail restart; do NOT claim the entire saved 53-second capture has
zero restarts. Local evidence:
`artifacts/mse-first-frame-normal-browser-20261005.json` and `.png` (the screenshot
is after that tail restart). The missing-first-frame fault itself is simulated,
not an injected browser-decoder fault; this smoke check verifies normal startup
does not falsely trip the new guard. No real camera, application session or
production credentials were used. Temporary tab and loopback fixture were closed.

Verified separate deployment:
`build-vs2026-stable/deploy/mse-first-frame-cbbf475693c1-web-8598be71d403`,
94 files / 177,889,238 bytes (`artifacts/mse-first-frame-package-verified-20261005.json`).
Native identity is unchanged because this is a client-only runtime fix. Actual
authenticated zero-camera restart/history checks use
`artifacts/run-mse-first-frame-runtime-20261005/` and pass both lifecycles in
18.460 seconds. Frozen OFF audit58671 remains live at 104/128 inspected,
zero failures so far; overall inspection, matched ON, latest-candidate endurance,
true hidden/resume and independent browser-profile gates remain open. No main
merge, push, private media tracking, production change or user backlog edit.

### Worker portability review (2026-10-05)

The existing Ubuntu WSL environment compiles and runs the actual worker/timer
shutdown regression with GCC 11.4, C++20, `-Wall -Wextra -Werror`, and libatomic.
Windows headers/priority calls are now guarded; Linux retains normal scheduler
policy without requiring privileges. The completion fence uses the standard
atomic fence, and a qualified queue type avoids GCC's member/type name conflict.
Both the held callback join and idle cancellation tests pass on Linux and Windows.
The pinned Windows server and ownership consumer build also succeeds. No packages
were installed and the managed dependency fingerprint remains unchanged.

This is component portability validation, not a complete Linux server build.
The additional Linux timestamp policy executable exposed an exact upper 5%
boundary rounding failure at TimestampRegressionGuardTests.cpp:215; that finding
must be resolved separately before the integration review is closed.

### Integration manual review and release candidate (2026-10-05)

User requested consolidation after manual review. The repository's mainline is
`master`, not `main`. The combined branch already contains the five ordered work
areas; this review does not blindly merge abandoned or unrelated branches.
The older cleanup lock commit is patch-equivalent to a change already retained.
Motion-calibration groundwork and the historical audio-first-pass branch are
not part of this integration and are left intact.

Review focused on the authentication/CSRF and telemetry ownership boundaries,
bounded asynchronous history storage, health cache freshness and shutdown,
PTZ original-transport Stop ordering and configuration fencing, worker lifetime
and cancellation, live mux/AAC packet ownership and pre-header teardown,
dependency-safe MSE eviction/presented-frame recovery, layout normalization and
group-scoped browser persistence, and managed dependency bootstrap guards.
No additional blocker was found in this pass beyond the timestamp policy's
cross-platform exact-boundary rounding. CadenceDrifted now compares the positive
integer difference against Duration/20, avoiding both floating-point boundary
differences and overflowing multiplication. Tests include both inclusive 5%
boundaries, adjacent rejected ticks, non-divisible durations and INT64 extrema.
This preserves the intended threshold rather than widening it.

Fresh checks on this candidate:

- Linux timestamp policy: GCC C++20, Wall/Wextra/Werror, pass.
- Pinned Windows server/UI and native regression targets: build pass.
- Windows timestamp, actual worker callback/idle cancellation, 20 cross-DLL
  Baichuan lifetime cycles, eight AAC media ownership/reconnect cycles, and four
  pre-header EOF teardown paths: pass.
- Policy/harness Python suites: 78 tests pass, two opt-in checks skipped. The
  first attempt used incorrect import paths and sandbox-restricted temp paths;
  only the corrected invocation is the qualified result.
- Frontend non-DVR suites: 86 pass, one optional offline export skipped. The
  broad invocation's DVR fixture initially could not launch its FFmpeg child;
  its isolated rerun passed. Existing Sass/chunk advisories remain.
- The frozen OFF candidate's 128-output strict decode/timestamp audit is now
  complete with all outputs passing, and its owning process exited zero. This
  is evidence for that frozen candidate, not latest-candidate endurance or a
  completed matched observer OFF/ON comparison.
- Fresh authenticated multi-client history/revocation/restart checks pass for
  both zero-camera server lifecycles in 18.334 seconds.
- Installed dependency inventory remains byte-identical to the baseline:
  2,673 files / 759,367,637 bytes; zero added, removed or changed files.

Separate byte/identity-verified release candidate:
`build-vs2026-stable/deploy/integration-reviewed-ba0ad903383b-web-430ccda99640`,
94 files / 178,003,926 bytes. Evidence is in
`artifacts/integration-reviewed-package-20261005.json`,
`artifacts/integration-review-build-20261005.log`,
`artifacts/integration-review-unit-tests-qualified-20261005.log`, and
`artifacts/run-integration-reviewed-runtime-20261005/`.
No new clip/image/database/dump assets occur in the integration diff, and neither
artifacts nor deployment directories are tracked. User BACKLOG.md edits remain
unstaged and unchanged. No production deployment or remote push was performed.

Remaining validation limits: no complete Linux server build, full latest-candidate
endurance, matched observer ON measurement, independently profiled browser
hidden/resume test, physical PTZ movement acceptance, or compiler-servicing ABI
comparison. Layouts remain group-scoped browser preferences, not shared server
configuration. These limits remain open after consolidation; do not mark the
overall goal complete or infer production stability from this release candidate.

### Genuine visibility evidence and measured-start gate (2026-10-05)

Separate branch `codex/real-browser-visibility-validation` adds actual document
visibilityState to the production-composable fixture's observations, and a
bounded 16-entry visibilitychange transition list. Computer-use skill and the
supported CUA browser APIs tested the fixed synthetic H264/AAC asset. Opening
another tab while the in-app browser was not visually presented did not produce
a hidden document: every observed state was visible and the transition list
remained empty. This is NOT a hidden/resume pass, and no property was mocked to
manufacture one. Ignored evidence:
`artifacts/real-visibility-attempt-20261005.json` and `.png`.
The finite 35-second fixture's expected tail watchdog restart remains disclosed.
The fixture compiled/type-checked; no production player source changed.

The completed frozen OFF audit made a matched ON attempt possible. First ON
attempt used the same package and sources but independent DOM inspection found
1280x720 viewport instead of the baseline 2172x1550. It was excluded, and only
verified disposable server process group20936 was gracefully signalled. Harness
22748 then exited1 with expected connection-refused cleanup; this is not a
spontaneous server crash or a measured comparison result.

An opt-in WITNESS_TEST_BROWSER_GATE_VIEWPORT now prevents measurement until an
independent recent DOM proof in the fresh run directory confirms exact viewport,
visible fullscreen layout, no activity strip, exact source dimensions, every
player inside the viewport, nonzero media time, unpaused readyState3/4 and no
media error. Proofs expire after60 seconds; malformed, incomplete or incorrect
proof cannot qualify. Eight profile/proof tests and twenty additional workload,
shutdown deadline and comparison tests pass (28 total). A sandbox temp-access
failure was rerun with scoped access; it is not counted as a code regression.

Corrected attempt94145 used the frozen native2213c5ef8142/webb301018e1023 package
and all five observersON, sixteen qualified repeated sources, one-fps CPU
inference/manual clips/DVR, scoped cached-summary timing and whole-recording
inspection. Its viewport was verified at2172x1550 BEFORE any proof was accepted.
The current platform applies the override to an existing test tab, not a future
tab; independent DOM checks caught and corrected that distinction. No proof was
accepted: startup playback deteriorated from5/16 initialized ready players to
0/16 readyState3/4, and the bounded five-minute gate timed out. No measured phase
was started. The owning handle exited1; cleanup also captured a slow-shutdown
dump. A complete latest-server shutdown regression is not inferred from this
older frozen candidate's resource-contended failure.

The fresh fixture's authenticated health snapshot shows hostCPU100%, RAM87%,
processCPU36.39% of host logical capacity, and all16 input streams connected,
accepting packets without reported video corruption/drops/cadence rejection.
Aggregate OS counters independently showed substantial CPU/memory consumption
from non-test applications. This run therefore cannot be compared with the
quieter OFF phase to attribute a CPU increment to diagnostic observers. It also
does not prove that external load is the only cause of the stalled playback.
Ignored evidence is under `artifacts/run-matched-cached-health-on-20261005-v2/`,
`artifacts/matched-cached-health-on-v2-startup-health-20261005.json`, and
`artifacts/matched-cached-health-on-v2-unready-dom-20261005.json`.
No other applications were stopped. All new owned browser tabs/fixture servers
are closed, and the temporary viewport override was reset. The matched ON gate
and genuine hidden/resume validation remain open; master/runtime/dependencies,
remote, production and user BACKLOG edits are unchanged by this validation piece.

### Retained browser proof in replay comparisons (2026-10-05)

Branch `codex/replay-layout-comparison-proof` retains the independently captured
DOM proof and its validation timestamp in the measured replay artifact, and
preserves both through summarization. The comparison revalidates the proof at
validation time and measurement start, requires an integer viewport with bounded
dimensions, and refuses an OFF/ON comparison with different observed viewports.
Missing proof, a future validation boundary, stale capture, hidden documents,
incomplete player membership or unready players produce an incomplete result.
Separate phase capture times are permitted; the actual viewport must match.

All 32 profile, summary and comparison tests pass, including negative cases and
legacy evidence remaining explicitly unavailable. Python sources parse and the
diff has no whitespace errors. No production source, deployable binary, web
bundle or dependency changed. The completed earlier OFF run lacks retained
startup proof and cannot be retroactively qualified by manufacturing it. Fresh
paired runs are required. Startup DOM proof does not establish uninterrupted
visibility, identical tile geometry throughout the phase, hidden/resume behavior,
or absence of external host contention. Those validation limits remain open.

### Loaded recording shutdown regression discovered (2026-10-05)

The consolidated native573cd0df694f/web e55b82803f31 package passed the isolated
zero-camera restart/history checks, but the subsequent mixed loaded test failed.
The first 90.180-second phase had 89 health samples (maximum gap1.054s), all four
inputs connected and no reported video corruption/drops. Three manual recordings
passed strict whole-file decode and timestamp inspection: H264 1280x720, HEVC
1920x1080 and HEVC 3840x2160. The fourth, synthetic H264/AAC 160x90, had no MP4
moov atom; independent ffprobe reproduced the error in its preserved fixture-owned
file. The process exited cleanly, but the second lifecycle was NOT reached.
This is a failed recording gate despite healthy live-stream counters.

Ignored evidence: `artifacts/run-consolidated-mixed-loaded-20261005/`, including
`recording-inspection-failed-7247bd10054ce423.json` and its private MP4. No media,
database, session credentials or dump is tracked. The Tapo-derived source is an
earlier saved OUTPUT replay, not raw physical camera/network input.

Branch `codex/finalize-active-recording-on-shutdown` explicitly invokes the
existing OnClipFinished(true) path on the camera worker before observer/filter,
input and live stream teardown. Previously WorkerShutdown left RecordStream
alive; its eventual destruction calls the Stream base destructor, not the
derived OutputStream::Shutdown trailer writer. The source-order regression
checks actual calls (comments removed); both shutdown-order tests pass. The
rebuild and actual recording regression remain pending: this fix is not yet
qualified as a deployable recording repair.

The fix rebuilt successfully and the separate package
`build-vs2026-stable/deploy/recording-shutdown-3b7916dff6bd-web-f5d7c333b90e`
passes byte/inventory/embedded-identity verification (94 files / 178,003,926 bytes;
`artifacts/recording-shutdown-package-20261005.json`). The focused actual-server
AAC recording regression then passed two 20-second replay lifecycles in77.261s.
Both manual clips and both continuous DVR segments passed strict whole-file
decode and packet/frame timestamp inspection with no diagnostics, missing,
duplicated or regressing timestamps. Evidence is in
`artifacts/run-recording-shutdown-aac-regression-20261005/`; both progress files
are inspectionComplete:true, completedOutputs:2/2, passed:true.

This focused pass repairs the demonstrated open-recording finalization path; it
does not qualify the full mixed CPU-inference workload. That exact four-source,
two90-second-lifecycle rerun is now active under
`artifacts/run-recording-shutdown-mixed-loaded-20261005/`. No result is claimed
until its recording inspection and both lifecycle exits complete. The fix remains
on its separate branch and has not been merged or pushed. Production and the
user-owned BACKLOG.md are untouched.

The mixed native3b7916dff6bd/webf5d7c333b90e rerun completed with exit0 in803.802s:
both 90-second lifecycles, actual CPU inference on all four sources, all16 manual
clip/DVR outputs strictly inspected, and both graceful exits passed. However,
review during the second lifecycle found unfinished Clip.Duration values in the
fixture database: first-lifecycle cameras1/3/4 remained zero while camera2 was91.
Media finalization therefore did NOT establish final metadata persistence.

The strengthened duration check passed a single-camera20-second trial (47.505s);
that setup can dispatch its one final message despite shutdown, so it is not a
negative regression. The two-camera AAC trial against the same media-only binary
failed the new runtime assertion in69.895s: camera1 duration20, camera2 duration0,
despite valid recordings. Evidence:
`artifacts/run-recording-shutdown-metadata-two-camera-red-20261005/`.

The pending follow-up serializes message dispatch with the shutdown drain. After
worker joins, it takes one finite queue snapshot and handles ONLY final clip
statistics, never start/configuration commands, without holding the queue mutex
during database work. It leaves a fresh wake message: the snapshot could consume
the original shutdown wake before a blocked MessageLoop::Pop sees it. A native
queue test checks FIFO snapshot ownership, late-message preservation and the
nonblocking empty case. Runtime evidence now requires a positive persisted
duration for every new clip, not merely rows/files. Sixteen pure workload and
source-order tests pass. The expanded native tests/build and multi-camera green
runtime check are still pending; no merge, push or production deployment.

The expanded Windows native queue/shutdown target now builds and runs successfully:
finite FIFO snapshot, late-message preservation, empty snapshot, actual held timer
callback joining and idle cancellation all pass. The full server build also
completed. Review additionally found motion-start code assigned INT64_MIN to
unsigned end timestamps, turning the sentinel into2^63 and defeating the manual
end max(). Those initializations now match ClipStatistics::Clear's zero end
sentinel. Seventeen pure regressions pass; metadata evidence now checks both
minimum and maximum durations against the replay window plus its bounded startup
allowance, rejecting huge positive values as well as zero/negative durations.
This last source change still requires a rebuild and runtime validation. No new
package or merge is inferred from the preceding native test pass.

The unsigned-sentinel rebuild completed successfully. Follow-up review found
that a zero/unset motion end during active motion would underflow the existing
unsigned subtraction in Recording.cpp. A small production ClipTiming.h policy
now treats an unset end as the recording end, retains an earlier real end for
quiet/grace tails, and clamps reversed/out-of-window motion boundaries before
subtracting. The actual native queue/shutdown target tests that production
function for active, quiet-tail, reversed/equal, unset and legacy-sentinel cases.
The persisted-recording gate also rejects missing, negative and excessive active
motion durations per clip; seventeen pure tests pass. Native policy tests and
the final rebuilt server runtime remain pending. No production deployment,
remote push, master merge or user BACKLOG mutation.

The final native d63a3532fc76/web48906d0cd45a build and package pass coherence
verification:94 files /178,030,038 bytes
(`artifacts/recording-metadata-package-20261005.json`). The actual native queue,
held-callback shutdown and ClipTiming policy tests run successfully on Windows
and existing Ubuntu GCC with C++20,-Wall,-Wextra,-Werror,-pthread,-latomic; no
Linux packages were installed. This is not full-server Linux validation.

The two-camera metadata regression now passes both20-second recording lifecycles
in79.878s: all8 clip/DVR files strictly inspected, positive total durations21/21
then20/20, no invalid motion durations, and clean shutdown/restart. The previous
binary demonstrably failed the same two-camera duration gate with20/0. Evidence:
`artifacts/run-recording-metadata-two-camera-green-20261005/`.
Authenticated multi-client health history, revocation and restart persistence also
pass on this final package in13.316s under
`artifacts/run-recording-metadata-history-final-20261005/`.

Final review covers finalization before camera state teardown, bounded queue
snapshot ownership, dispatch/drain serialization, preservation of a shutdown wake,
no new camera actions during drain, unsigned timestamp seeds and motion-duration
clamping. The earlier native3b7916dff6bd mixed H264/HEVC/AAC two-lifecycle media
pass is retained as narrower evidence, not mislabeled as latest-candidate mixed
load/MSE playback acceptance. Genuine hidden/resume, physical PTZ and matched
full-load observer overhead remain open. No private recordings, DBs or dumps are
tracked; user BACKLOG edits and production remain untouched.

Final merged-candidate mixed recording validation (2026-10-05):

Native `d63a3532fc76` / web `48906d0cd45a` passes the isolated four-source,
two-lifecycle CPU-inference/manual-recording/DVR regression in 583.414 seconds.
Evidence: `artifacts/run-final-master-mixed-recording-20261005/`.
Both 90-second measured phases retain 89 samples, with maximum gaps of
1.042 / 1.045 seconds. Every sampled camera state is Connected; reported video
drop and corruption counters stay zero. All four cameras complete inference in
each lifecycle. All 16 saved clip/DVR files pass whole-recording strict decode
and packet/frame timestamp checks. Persisted clip durations are 91/91/91/90
seconds, then 91/91/91/91; invalid motion-duration counts are zero throughout.
The fixture completes both graceful shutdowns and restart with test exit 0.

Coverage remains four paced loopback replay sources: saved Tapo H.264 720p
OUTPUT, normalized HEVC 1080p/2160p fixtures, and small synthetic H.264/AAC.
It does not prove raw Tapo network delivery, perceptual pacing, MSE playback,
motion-trigger activation, physical PTZ or the matched 16-player overhead gate.
Fresh browser inventory exposes only in-app Browser and MCP Apps, not an
external Chrome/Edge profile; genuine hidden/resume acceptance remains open.
The local master consolidation preserves native identity and user BACKLOG edits;
no remote push, production deployment or private media commit was performed.

Final-candidate browser endurance checkpoint (2026-10-05, still running):

`artifacts/run-final-master-16-mse-observers-off-20261005/` uses the verified
native d63a3532fc76/web48906d0cd45a package, 16 repeated qualified logical
sources, real MSE players, CPU inference and clip/DVR recording. Independent
2172x1550 fullscreen DOM proof passes the startup gate. Session 5476 is live;
there is no completed 30-minute or recording-audit result yet.

Actual persisted browser heartbeats now capture a genuine hidden interval,
unlike the earlier unsuccessful hide attempts: camera 1's 19:08:51.883Z sample
is hidden/paused at media time113.852004; the 19:09:23.822Z sample remains
hidden with the playhead evicted behind retained media (next start119.076933).
At 19:09:26.865Z it is visible/unpaused, but still behind buffered start122.076688.
All 16 players subsequently reconnect together for drift recovery around
19:09:29Z. The known hidden-buffer retention policy explains the gap; this is
not evidence of simultaneous input disconnection. A later direct authenticated
fixture health snapshot reports all 16 input cameras Connected and video
drop/corruption counters zero. It does not establish incident-time CPU causality.
The run is therefore not an uninterrupted-visible full-load benchmark and must
not be used to qualify a matched observer-overhead comparison as such.

Branch `codex/mse-hidden-eviction-resume` adds immediate resume recovery when
retention has evicted the playhead. Only verified safe RAPs inside retained
media may be seek targets; otherwise reconnect without the extra drift-wait
interval. Unknown, empty, malformed and exhausted ranges do not invent eviction.
Ten focused visibility/eviction policy tests pass. Type-check, rebuilt bundle
and genuine hidden/resume runtime acceptance remain pending; the running frozen
fixture and master package are unchanged. No production/session mutation,
private recording commit or user BACKLOG edit.
