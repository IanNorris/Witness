# Copilot Instructions — Witness

## Build

**Solution:** `Witness.sln` — Visual Studio 2022 (v143 toolset), x64 only.

```
MSBuild Witness.sln /p:Configuration=Release /p:Platform=x64
MSBuild Witness.sln /p:Configuration=Debug /p:Platform=x64
```

**PDB lock errors (C1041):** Kill `cl.exe` processes and delete `vc143.pdb` before rebuilding. DLLs are locked while the server is running — stop `witnessserver.exe` before building.

**Post-build:** `DeployBuild.bat` copies FFmpeg/OpenCV/libsodium DLLs and the `Web/` folder to the output directory automatically.

**No test suite exists** in this repository.

## Architecture

Witness is a video surveillance system with motion-triggered recording, HLS live streaming, and multi-platform clients.

### Components

| Project | Type | Language | Purpose |
|---------|------|----------|---------|
| **WitnessCamera** | DLL | C++20 | Video capture/encoding via FFmpeg 7.1, frame processing pipeline, HLS segmentation |
| **WitnessServer** | Console EXE | C++20 | REST API (Crow), message bus, camera worker threads, serves web UI |
| **MobileClient** | Flutter app | Dart | Cross-platform mobile client |

### Data flow

1. **InputStream** connects to camera RTSP stream, reads packets via FFmpeg
2. **CameraWorker** (inherits `WorkerBase`) owns the stream lifecycle and filter chain
3. **MotionFilter** / **PersonRecognitionFilter** analyze frames for motion events
4. **OutputStream** records clips to disk on motion detection (with configurable lead-in)
5. **LiveOutputStream** segments video into HLS partials (0.33s target) for low-latency streaming
6. **MessageBus** passes typed `Message` objects between worker threads using mutex + condition_variable queues
7. **WitnessListener** serves REST API via CppREST SDK `http_listener`, routing through `IListenerCommand` subclasses

### Web frontend

Knockout.js 3.4.2 MVVM app served from `WitnessServer/Web/witness/`. ViewModels in `asset/js/viewModels/`. Uses HLS.js for live video, jQuery, Bootstrap, DataTables, FullCalendar.

### Configuration

- **Server config:** JSON file based on `Server.json.template` — hostname, port, TLS, video processing settings
- **Runtime data:** SQLite database at `%ProgramData%\Witness\server.db`

## Dependencies

- **FFmpeg 7.1** — pre-built binaries in `ThirdParty/ffmpeg-7.1-win64/` (not a git submodule)
- **OpenCV 4.0** — git submodule in `ThirdParty/SubModules/opencv/`
- **libsodium** — git submodule for cryptography/auth
- **CppREST SDK** — via vcpkg manifest (`vcpkg.json`) and git submodule- **bgslibrary** — background subtraction algorithms (submodule)
- **SQLite** — bundled in `ThirdParty/SQLite-20180118/`

## Conventions

### C++ (WitnessCamera, WitnessServer)

- **C++20** standard (`/std:c++20`)
- **Pimpl pattern** on `Stream` and `RecordFilterBase` classes for ABI stability
- **`string_t`** (CppREST SDK) for all string handling — wraps `std::wstring` on Windows. Use `_T()` macro for literals. Console output via `tcout`/`tcerr`.
- **Smart pointers everywhere:** `shared_ptr` for shared ownership (streams, filters, global context), `unique_ptr` for sole ownership (commands, format contexts). No raw owning pointers in public APIs.
- **Worker thread pattern:** Inherit from `WorkerBase`, override `WorkerInit()`, `WorkerMain()`, `WorkerShutdown()`. Workers communicate via `MessageBus`.
- **Command pattern for REST routes:** Each top-level path segment maps to an `IListenerCommand` subclass. Sub-routing uses `ChildPath` vector.
- **CAMERA_API export macro** on all public WitnessCamera classes
- **Namespace:** `Witness::Camera` for the camera DLL
- **Thread safety:** `std::mutex` + `std::lock_guard` for shared state. `std::condition_variable` for queue blocking. Segment access in `LiveOutputStream` is always mutex-locked.
- **FFmpeg lifecycle:** `avformat_network_init()` called once in `Stream::OneTimeInit()`. Format/codec contexts freed in destructors. Custom log callback via `av_log_set_callback()`.
- **Raw pointer hazard:** Camera reconnect replaces `InputStream` while `OutputStream` may still hold a reference. Use cached values (e.g., `ID.Timebase`) instead of calling methods on the old stream.

### Web (Knockout.js)

- MVVM ViewModels as constructor functions with `ko.observable()` / `ko.observableArray()`
- Custom binding handlers for HLS streams: `ko.bindingHandlers.hlsPreview`, `ko.bindingHandlers.hlsStream`
- Templates loaded dynamically via jQuery `.load()` from `asset/templates/`
- Fullscreen camera view toggled via `witness.fullscreenMode` observable — adds `witness-fullscreen` class to `<body>`, hides navbar/sidebar/panels, CSS flexbox auto-tiles cameras

### HLS streaming

**Server-side (C++):**
- `LiveOutputStream` uses a single `AVFormatContext` with `empty_moov+frag_custom+dash` movflags
- Init segment stored separately with generation counter; AVIO swapped per segment
- Minimum 1-second segment duration enforced (Tapo cameras send keyframes every ~50-100ms)
- Audio uses independent DTS normalization (`_InitialAudioDTS`) with `stream_index=1`
- Backlog of ≥15 segments maintained for client catch-up
- `av_write_frame` does NOT take ownership of packet data — always `av_packet_unref` after. `av_interleaved_write_frame` DOES take ownership.

**Client-side HLS watchdog (`cameraStream.js`):**
- **Do NOT seek the playhead from outside HLS.js.** HLS.js has its own `bufferSeekOverHole` and `bufferNudgeOnStall` recovery. External seeks fight these mechanisms and cause thrashing loops (seekToLive ↔ bufferSeekOverHole infinite cycle).
- Spinner/connection-lost indicators are **poll-based** (250ms interval checking `lastFragTime`), not event-driven. HLS.js events like `BUFFER_STALLED_ERROR` don't fire in all stall scenarios.
- Spinner shows when a fragment arrived within the last 3s (`HLS_SPINNER_TIMEOUT_MS`). Segments arrive in ~2s bursts — threshold must exceed segment interval to avoid flicker.
- Stream restarts (`hls.destroy()` + recreate) after 5s with no fragments (`HLS_RESTART_TIMEOUT_MS`), or after 5s with no initial fragment (`initialTimeout`).
- Stuck detection: if `readyState ≤ 1` persists while not paused, triggers restart with **exponential backoff** (3s → 6s → 10s cap). Backoff resets when `readyState ≥ 3`. This prevents restart churn on streams with misaligned timestamps (e.g., pre-recorded content served as live).
- `restartStream()` resets both `lastFragTime` and `streamStartTime` to re-arm the watchdog.

**Client-side HLS diagnostics (`cameraStream.js`):**
- `StreamDiagnostics` object per camera: timestamped event log (start, frag, error, stall, restart, stuckRestart, initialTimeout), rolling stats (restartCount, stallCount, errorCount, latency min/max, totalFragments), and `snapshot()` for live video element state.
- Events pruned to 24h max. Global map at `window._witnessDiag[cameraID]` for DevTools access.
- `window._witnessDumpDiag()` downloads all diagnostics as JSON. Download button on preview page near scale controls.
- `hlsStream` binding uses `cameraID + '_stream'` suffix in diagnostics to distinguish from preview.
