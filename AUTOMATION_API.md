# Scoped automation API

Automation keys are separate from browser sessions. They work only on the three
`/api/v1` endpoints below; they cannot fetch thumbnails, video, live streams,
DVR segments, or the web UI. Send the key in `Authorization: Bearer ...`, never
in a URL. Use HTTPS (or a trusted private tunnel) so it is not exposed on the
wire.

An administrator creates a key with `POST /api/keys/create` using their normal
session cookie and the CSRF token returned by `/auth/profile`:

```json
{
  "csrf": "...",
  "name": "Front PIR bridge",
  "allowedCidrs": "192.168.85.12/32,127.0.0.1/32",
  "scopes": ["health.read", "clips.read", "record.trigger"]
}
```

The 256-bit key is returned once and only its hash is stored. `GET /api/keys`
lists names, scopes (bitmask: health=1, clips=2, record=4), source allowlists,
usage counters and revocation state. `GET /api/keys/audit` returns the last 200
successful requests; the audit table retains approximately the latest 50,000.
Revoke with `POST /api/keys/revoke` and `{"csrf":"...","id":123}`. Keys also
stop working when their owner is disabled or loses administrator privilege.
IP checks use the socket peer, not `Forwarded` or `X-Forwarded-For`; requests
carrying forwarding headers are rejected. Use an individual key for each source.

## Key endpoints

| Scope | Method and path | Result |
| --- | --- | --- |
| `health.read` | `GET /api/v1/diagnostics/health` | The existing health snapshot JSON. |
| `clips.read` | `GET /api/v1/clips/search` | Metadata only, limited to cameras the key owner can access. |
| `record.trigger` | `POST /api/v1/cameras/{id}/record` | Starts or stops manual recording for an accessible camera. |

Clip search accepts `from`, `to` (Unix seconds), `camera`, `tag` (one exact tag
name), `minDuration`, `limit` (1–100, default 50), and `offset`. It returns
clip IDs, timestamps, camera, duration, record mode, motion score, saved flag,
description, and tag names. It does not return URLs or image/video bytes.

The recording request body is `{"record":true,"tags":["pir:driveway"]}` or
`{"record":false}`. Tags are optional, up to eight 64-character names using
letters, digits, `_`, `-`, `:` or `.`. They are attached to the newly created
manual clip and survive detector retagging. Start returns `409` if the camera
is already recording, so a trigger cannot silently tag the wrong clip; stop
returns `202` after the command is queued. The caller should set a stop timer
or send `record:false` when its PIR event ends.

This API intentionally does not bypass browser authentication or authorize
media access, even in a debug build.
