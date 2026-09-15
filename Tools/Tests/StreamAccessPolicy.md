# Stream access regression checks

In a VS2026 developer shell, compile the policy tests in both configurations:

```powershell
cl /nologo /std:c++20 /EHsc /c Tools/Tests/StreamAccessPolicyTests.cpp /Fobuild-vs2026/StreamAccessPolicyRelease.obj
cl /nologo /std:c++20 /EHsc /D_DEBUG /c Tools/Tests/StreamAccessPolicyTests.cpp /Fobuild-vs2026/StreamAccessPolicyDebug.obj
```

HTTP integration matrix (use an existing camera and valid segment URLs):

| Request | Release / RelWithDebInfo | Debug |
|---|---|---|
| No session, direct IPv4/IPv6 localhost | 403 | Allowed |
| No session, remote peer | 403 | 403 |
| No session, loopback with Forwarded / X-Forwarded-For / X-Real-IP | 403 | 403 |
| Valid session without camera group membership | 403 | 403 remotely |
| Valid session with camera group membership | Allowed | Allowed |

Exercise `/stream/<camera>`, initialization (`/0/i?g=...`), full (`/<segment>/f`),
and partial (`/<segment>/<part>`) URLs independently. Exercise both WebSocket
routes (`/ws/stream/<camera>` and `/ws/stream/sub/<camera>`); denied handshakes
must not upgrade. Request headers must not be able to spoof the socket peer.

The Debug exception is only for live streams; it does not bypass account,
administration, clip, or other API authentication. Do not expose Debug builds
through a local reverse proxy: a proxy that omits forwarding headers cannot
be distinguished from a direct local client. Production builds never bypass
stream authentication. Existing WebSocket connections need to reconnect to
exercise the new handshake checks.
