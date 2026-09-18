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
- V1 merges server history with the current browser and clearly marks other
  clients unavailable. Cross-client reporting can later use ephemeral,
  authenticated client summaries without uploading raw media.
- JSON contains the complete correlated evidence, collection bounds, clock
  offset/uncertainty, versions, effective policies, selected detailed traces,
  and missing-data reasons. CSV/TSV contains the copyable per-camera table and
  protects against spreadsheet formula injection.
- Serialization copies bounded snapshots and never holds the global context,
  stream, or processing locks while producing the response.

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
