# Dashboard layout validation checkpoint

The layout data and component-script regressions cover legacy migration,
priority-generated presets, custom geometry preservation, rectangular activity
placement/overlap guards, automatic slot priority/fallbacks and motion holds.
The current UI suite has 41 passing tests. These tests are not live-stream or
hardware-decoder validation.

On 4 October the built Web bundle was opened in the in-app browser against
`web-ui/tests/fixtures/ptzDashboard.mjs --serve --layout --auto-slots`.
This fixture binds only loopback, uses synthetic JPEGs and cannot forward any
camera command. The browser showed:

- Camera 10 fixed; cameras 12/13 occupying the two designated fallback slots.
- Synthetic motion on cameras 14/15 promotes higher-priority 15 into Auto 1
  and 14 into Auto 2 without displacing fixed camera 10.
- After both motion flags clear and the configured six-second hold expires,
  cameras 12/13 return to Auto 1/Auto 2.
- All five live camera image elements remain mounted. Inactive images occupy
  inert two-pixel offscreen regions; visible ones occupy the saved slot bounds.

This last check directly closes the earlier unobserved all-quiet fallback case.
It does not establish MSE decoder continuity or precise hold timing in a real
browser: tool round trips exceeded the six-second hold. Timer ordering is
covered by the component regressions instead.

Screenshot: `build-vs2026/dashboard-ui-checks/automatic-slots-all-quiet.png`.
The browser tab and both temporary fixture listeners were stopped after testing;
the first listener was replaced because its noninteractive stdin was closed.
No actual camera, authentication setting or production database was changed.

## Real MSE player continuity checkpoint (4 October)

The fixture now accepts `--mux` to replay private saved native mux output through
the actual built dashboard and MSE player. Five synthetic camera IDs use the
same saved Tapo 8 video; this is not five independently captured input streams.
It shares its MP4 parser, verified binary transport and cancellable timers with
`tests/replayMux.mjs`. It never connects to Witness or a camera. The loopback
listener expires after four minutes. Optional `--evidence` writes bounded,
five-second snapshots for offline verification.

```powershell
# From web-ui; camera motion is controlled with JSON lines on fixture stdin.
node tests/fixtures/ptzDashboard.mjs --serve --layout --auto-slots --mux ../artifacts/tapo8-rate-consistency-15minute-20261004.mp4 --evidence ../artifacts/dashboard-mse-slots-20261004.ndjson
# After opening the displayed loopback URL in a browser, set both active:
# {"cameraId":15,"active":true}
# {"cameraId":14,"active":true}
# Later send corresponding active:false lines and observe fallback.
node tests/verifyDashboardMse.mjs ../artifacts/dashboard-mse-slots-20261004.ndjson
```

The recorded in-app-browser run supplied 29 snapshots over 139.999 seconds.
Every snapshot contains all five ready-state-4, unpaused players, with identical
per-player start times throughout. All five clocks advance between samples,
including inactive offscreen players; final media times range 143.229–143.320 s.
Each camera opened exactly one stream socket and closed none during observation.
All samples report zero restarts, stalls, errors, integrity mismatches, decode
corruption and append-queue depth. DOM evidence shows higher-priority camera 15
in Auto 1 and 14 in Auto 2, then designated fallbacks 12/13 returning. Camera 10
remains visible throughout. This closes the earlier MSE slot-transition gap.

Evidence: `artifacts/dashboard-mse-slots-20261004.ndjson`; screenshot:
`artifacts/dashboard-mse-slot-recorded-proof.png`. All 44 UI tests pass in
`artifacts/dashboard-slot-ui-tests.log`, including new replay transport tests.
Both browser tabs and listeners were closed; no physical commands were sent.

Limitations: no real network-arrival jitter, hardware decoder capacity comparison,
hidden-browser-tab throttling or production load is reproduced. This validates
component/player continuity, not production stability. Exact six-second hold
timing remains covered by deterministic component tests rather than these
five-second snapshots. Runtime/package files are unchanged from the combined
`ordered-improvements-b58eba2` build.
