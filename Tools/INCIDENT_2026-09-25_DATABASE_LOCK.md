# Production database-lock and cleanup incident, 2026-09-25

Evidence: user-supplied `witness-2026-09-24.log` (603 MB),
`witness-2026-09-25.log` (3.43 GB), and
`witness-health-2026-09-25T22-24-18-310Z.json`. Log times are server local;
the health export is UTC. The analysis streamed the log line by line rather
than loading it into memory.

## Timeline and counts

- Missing detection JPEGs were already being retried continuously at the
  beginning of 24 September. The 25 September log contains 1,776,667
  `Failed to inspect detection asset` warnings and 304,426 failed attempts to
  reprocess the same clip, UID 2784760. On Windows, `is_directory(path, ec)`
  set `no_such_file_or_directory` for these absent files; cleanup treated that
  as a failure rather than a successful already-deleted asset.
- At 00:22 on 24 September, startup logged a SQLite `disk I/O error` while
  enabling WAL, then continued and completed startup maintenance by 00:23.
  This earlier event warrants separate attention; it does not establish the
  cause of the next day's lock.
- The first `database is locked` errors on 25 September appear at 11:22:23,
  in `UpdateClip`, tag, and audio-event writes. `CreateClip` began failing at
  11:22:25 and `CreateContinuousSegment` at 11:23:57. These failures can leave
  actual recording files without corresponding indexed rows.
- At 11:26:55, quota enforcement attempted to delete a continuous-segment
  row, received `database is locked`, but reported one deletion. It had
  removed the file before the unsuccessful database deletion.
- At 11:31:55, the next quota pass selected the same stale row. The on-disk
  file size was now zero, so its `while (totalSize > quotaBytes)` loop made no
  progress when the DB deletion failed. It emitted errors until 23:25.
  The 25 September log contains 27,149,938 `database is locked` lines,
  including 26,919,318 for `DeleteContinuousSegment` alone.
- At the 22:24 UTC health snapshot, the HTTP server and the visible browser
  players were responding. It did not capture the initial SQLite lock holder
  or establish when clip files stopped being written. Tapo video corruption
  appears concurrently but is a separate packet/decode issue.

## Code changes on this branch

- A missing managed JPEG now counts as successfully removed, after the
  existing cache-root and camera-directory safety validation. A regression
  test requires its pending row to clear.
- Continuous quota cleanup subtracts the row's stored `FileSize`, not the
  current physical file size. It stops on a failed DB delete and is capped at
  100 rows per timer pass, preventing a CPU/logging runaway.
- Retention and emergency cleanup also check DB deletion results instead of
  claiming failed deletions succeeded; genuine filesystem removal errors are
  not treated as missing files.
- Failed prepared statements are reset immediately, and future SQLite error
  lines include primary and extended result codes to help locate the initial
  lock cause.

The original reason SQLite first became locked at 11:22 remains unproven.
Avoid attributing it to the missing JPEGs or the quota loop: the former
predated it and the latter began several minutes later. The quota loop was a
severe amplifier. Check the production DB and its WAL/SHM files together,
along with Windows disk/SQLite I/O health, before attempting repair or
manual cleanup.
