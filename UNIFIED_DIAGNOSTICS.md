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
