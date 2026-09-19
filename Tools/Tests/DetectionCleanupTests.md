# Detection cleanup safety test

Build the optional `DetectionCleanupTests` target and run the resulting executable.
The test uses an in-memory SQLite database and its own temporary cache directory;
it never opens the configured Witness database or cache.

It advances the clock 30 days, derives a three-day retention cutoff, and verifies
that only eligible rows strictly before that cutoff are deleted. Coverage includes
clips, DVR segments, verified face crops, an exact-cutoff row, newer rows,
cross-camera paths, shared assets, a full page of protected candidates, cursor
resume after restart, and a recently rewritten file awaiting a later retry.
