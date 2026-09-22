# Opt-in live packet capture

The administrator-only `POST /debug/packet-capture` endpoint starts one bounded
capture for a live camera tier. Send JSON containing the normal administrator
CSRF token, `cameraId`, `tier` (`main` or `preview`), and optionally
`durationSeconds` (1–30, default 20). Camera 8's `preview` tier is the first
useful target for the Tapo failure. The response names a local directory under
the configured cache path's `packet-captures` directory. The endpoint never
serves the captured media over HTTP.

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
