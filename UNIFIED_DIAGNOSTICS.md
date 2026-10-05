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
