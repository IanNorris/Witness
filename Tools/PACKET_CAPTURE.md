# Opt-in live packet capture

The administrator-only `POST /debug/packet-capture` endpoint starts one bounded
capture for a live camera tier. Send JSON containing the normal administrator
CSRF token, `cameraId`, `tier` (`main` or `preview`), and optionally
`durationSeconds` (1–30, default 20). Camera 8's `preview` tier is the first
useful target for the Tapo failure. The response names a local directory under
the configured cache path's `packet-captures` directory. The endpoint never
serves the captured media over HTTP.

This endpoint always requires a valid enabled administrator session and its
matching CSRF token, including in Debug builds and for loopback requests. The
loopback exception available to direct live-stream playback does not apply.

From the authenticated administrator dashboard, the browser console can start
the camera 8 preview capture without restarting the server:

```js
const profile = await fetch('/auth/profile', { method: 'POST' }).then(r => r.json())
const response = await fetch('/debug/packet-capture', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ csrf: profile.csrf, cameraId: 8, tier: 'preview', durationSeconds: 20 }),
})
console.log(response.status, await response.text())
```

The capture contains `events.jsonl`, `input.bin`, and `output.bin`:

- `input` events index exact encoded AVPacket payloads immediately after
  `av_read_frame`, with source timestamps, timebase, flags, codec, and FNV-64 hash.
  `stream` and `extradata` events preserve the codec setup for replay/decoding.
- `decision` events correlate by `activityId` and payload hash, and include the
  live mux's written/dropped decision and normalized timestamps.
- New captures also record independent monotonic `observedSteadyUs` on input,
  plus `muxObservedSteadyUs` and `decisionSteadyUs` on decisions. These identify
  demux arrival gaps versus time inside Witness; demux-to-mux delay includes
  the deliberate one-video-packet duration lookahead. The clock has no UTC
  meaning and is comparable only within the same host/process capture.
- `init` and `partial` events index the exact fMP4 bytes published by the live
  mux, including generation, segment, part, independent flag, and hash.

The capture stops after 30 seconds at most or 128 MiB of accepted binary data.
An 8 MiB asynchronous writer queue limits camera-thread stalls. If that queue
fills, capture stops and `queueRejectedRecords` is nonzero; do not draw packet
loss conclusions from a truncated capture. The files contain real video/audio
and must be handled as sensitive recordings. They are not removed automatically.

Inspect and optionally reconstruct the published output:

```powershell
python Tools/packet_capture_report.py "X:\WitnessCache\packet-captures\camera-8-preview-..." --replay tapo-output.mp4
```

This captures demuxed encoded access units, not RTSP/RTP network packets. If the
demuxed input is already damaged, a separate network-level capture would be the
next layer to inspect. A byte-for-byte comparison of whole input packets to
fMP4 chunks is invalid because the muxer adds container framing; correlate
packet hashes and decode/timestamp decisions, then inspect the replayed output.

The report prints delivery FPS, arrival-gap percentiles and demux-to-mux /
mux-to-decision percentiles when independent observation timestamps exist.
Old captures report timing unavailable rather than estimating it from the
possibly faulty source DTS. A demux observation is still not NIC/RTP ingress.

For local camera testing without starting the server, build
`Tools/Tests/LivePreviewProbe.cpp` against the matching Camera DLL and run
`Tools/Tests/run_live_preview_probe.py --help`. The runner reads one preview URL
from the specified SQLite database in read-only mode, closes the connection,
passes credentials only in the child's environment and filters arbitrary
native logs. It refuses existing output names and enforces a hard timeout.
It does not bypass the authenticated HTTP capture endpoint.
The probe now continues through mux generation changes, saving each new
generation in a separate file instead of concatenating incompatible timelines.
Use `--capture-delay` for an additional bounded 30-second raw packet capture.
All generated recordings are sensitive local artifacts and are not uploaded.

For an explicitly controlled stall test, the runner accepts a paired
`--pause-at SECONDS --pause-for SECONDS` (pause length 1..15). This sleeps only
the standalone probe's processing loop; it does not change camera settings,
pause production, or bypass authentication. Leave at least five seconds for
recovery. Each mux generation is written independently. Inspect every output
generation for both decode errors and presentation timestamp problems; a zero
process exit alone does not prove clean decoded-frame pacing.
