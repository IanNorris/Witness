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
