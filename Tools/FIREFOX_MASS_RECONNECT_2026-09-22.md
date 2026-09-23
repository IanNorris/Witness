# Firefox mass playback restart, 2026-09-22 23:53 UTC

Evidence: production health export `witness-health-2026-09-22T23-53-50-732Z.json`
and Firefox console export `MediaErrors.txt`, both supplied by the user from
the production server share.

At approximately 23:53:22 UTC, all seven active MSE players in Firefox 156
reported `InvalidStateError` on `SourceBuffer.appendBuffer()` within about
0.1 seconds. Witness restarted the players *after* five append errors each;
their WebSockets closed as part of those restarts. The players recovered about
3–8 seconds later. There were near-simultaneous `catchUpStart` events just
before the append failures.

The Firefox console has seven `Media resource blob:... could not be decoded`
messages, corresponding to the seven failed players. One includes
`NS_ERROR_DOM_MEDIA_FATAL_ERR (0x806e0005)`. Its detailed error message was
suppressed because `privacy.resistFingerprinting` was enabled. An autoplay
warning is also present but does not appear related.

The health snapshot shows all 11 camera workers and their main/preview streams
connected, empty processing queues, no server media events in the precise
failure window, and no slow HTTP handlers. All seven selected browser streams
were H.264 (six sub-streams and one main stream); the event cannot
be attributed to HEVC alone. The client and served web bundle hashes matched.

Interpretation: the Firefox media pipelines failed together; the synchronous
append errors were likely downstream symptoms. The fatal error code does not
identify whether the trigger was a Firefox media/graphics process reset, a
decoder resource failure, or a malformed-media condition. The synchronized
timing and healthy server streams favour a shared client-side failure, but do
not prove one. Do not change server packet handling on this evidence alone.

Next capture/experiments:

1. Use Firefox `about:logging` with the `media-playback` preset before a
   recurrence. Preserve the process logs and note the exact UTC time. Firefox
   documentation: https://firefox-source-docs.mozilla.org/xpcom/logging.html
2. Check `about:support` Graphics and Crash Reports after recurrence; compare
   the same seven-camera layout in Edge/Chrome.
3. Add client diagnostics for `MediaSource.sourceBuffers` membership, media
   element `error`/`emptied`, SourceBuffer `error`/`abort`, and watchdog
   event-loop gaps. On `InvalidStateError`, restart the unusable pipeline
   immediately instead of retrying four further appends.

The MSE prepare-append rules are documented at
https://w3c.github.io/media-source/#sourcebuffer-prepare-append . Firefox's
`NS_ERROR_DOM_MEDIA_FATAL_ERR` is used for more than one decoder/MSE error;
its MediaSource decode-error path is visible at
https://searchfox.org/firefox-main/source/dom/media/mediasource/MediaSource.cpp .
