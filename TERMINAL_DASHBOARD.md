# Terminal dashboard architecture

The first implementation is an in-process Windows console dashboard. It is
enabled automatically only for a normal interactive `WitnessServer.exe` run.
Windows service mode, redirected output, setup/test commands, and Linux retain
line-oriented logging. Pass `--plain-console` or `--no-dashboard` to opt out.

The rotating file log remains the authoritative Debug-level record. The
dashboard replaces only the interactive console sink and consumes a bounded
copy of warning/error events, so rendering cannot back-pressure camera work.
`--verbose` and `--debug-console` widen the dashboard event feed.

## Current screen

The overview displays build and uptime, connected/total cameras, host memory,
aggregate essential/AI queue depth and age, and stable camera rows containing:

- connection, motion and recording state;
- main codec/resolution and stream-established state;
- preview connection and retained HLS segments;
- established-period packet drops and timestamp repairs, plus lifetime
  corruption and reconnect counters (marked separately in the header);
- essential and AI queue depths.

Up/down selects a camera and filters the event pane. `A` returns to all-server
events. Ctrl+C retains the existing orderly shutdown behaviour. The screen is
redrawn after terminal resizing and its alternate buffer/cursor state is
restored on shutdown.

Camera attribution currently recognizes central lifecycle messages, explicit
`Camera N`/`source N` text, and camera-name prefixes. New asynchronous media
logs should migrate to an explicit structured camera ID rather than adding more
string patterns.

## Shared model and future standalone Windows app

`OperationalStatus` is deliberately independent of console APIs. The direct
dashboard obtains it in process. The next Windows presenter should be a small
`WitnessTui.exe` connecting to:

`\\.\pipe\Witness.Health.v1`

Use a read-only, versioned protocol with a four-byte little-endian length and
UTF-8 JSON payload. Message types should include `hello`, `snapshot`, `log`,
`gap`, and `heartbeat`. Each client needs a bounded output queue, maximum
message size, selectable interval/severity/camera filter, and an explicit gap
message after dropped updates.

Create the pipe with `PIPE_REJECT_REMOTE_CLIENTS` and an ACL limited to
LocalSystem, Administrators, the service identity, and configured local users.
Do not expose credentials, stream URLs, or unredacted paths. Any future control
operations belong on a separate authenticated protocol.

The health HTTP route and both terminal modes should ultimately use one fuller
`HealthSnapshotProvider`; the initial operational snapshot is intentionally
small and never copies diagnostic packet/event histories during its 2 Hz poll.

## Linux boundary

The operational structs and future protocol remain portable. On Linux the
dashboard factory is currently a stub (`IsSupported()` is false), leaving the
existing standard logging untouched. A later ncurses presenter can consume the
same model, with a protected Unix socket such as `/run/witness/health.sock` for
the standalone mode.

## Next diagnostic fields

The most useful additions, based on recent incidents, are interval deltas and
ages rather than lifetime totals:

- host CPU and Witness private/working memory;
- state-transition age and last reconnect reason/time/downtime;
- last input packet/frame/keyframe and published segment/partial ages;
- measured frame rate, HLS generation, client count and selected tier;
- reconnect/restart/stall deltas over one and five minutes;
- A/V skew, phase correction/saturation, and latest correlated media event;
- separate bounded lifecycle/anomaly events from rate-limited FFmpeg message
  aggregates, so repetitive mux warnings cannot evict disconnect evidence.
