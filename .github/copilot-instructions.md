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
| **WitnessServer** | Console EXE | C++20 | REST API (CppREST SDK), message bus, camera worker threads, serves web UI |
| **WitnessClient** | WinForms EXE | C# (.NET 4.7.2) | Desktop monitoring app with login and motion notifications |
| **MobileClient** | Flutter app | Dart | Cross-platform mobile client |
| **AndroidClient** | Gradle project | Java/Kotlin | Android client with Firebase Cloud Messaging |

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

- **Server config:** JSON file based on `Server.json.template` — hostname, port, TLS, Azure Vision, FCM, video processing settings
- **Runtime data:** SQLite database at `%ProgramData%\Witness\server.db`

## Dependencies

- **FFmpeg 7.1** — pre-built binaries in `ThirdParty/ffmpeg-7.1-win64/` (not a git submodule)
- **OpenCV 4.0** — git submodule in `ThirdParty/SubModules/opencv/`
- **libsodium** — git submodule for cryptography/auth
- **CppREST SDK** — via vcpkg manifest (`vcpkg.json`) and git submodule
- **bgslibrary** — background subtraction algorithms (submodule)
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

### C# (WitnessClient)

- WinForms with async HTTP via `HttpClient`
- REST calls through `WitnessRest.SendQuery()` helper
- Session token + CSRF token authentication

### Web (Knockout.js)

- MVVM ViewModels as constructor functions with `ko.observable()` / `ko.observableArray()`
- Custom binding handlers for HLS streams: `ko.bindingHandlers.hlsPreview`, `ko.bindingHandlers.hlsStream`
- Templates loaded dynamically via jQuery `.load()` from `asset/templates/`

### HLS streaming

- `LiveOutputStream` uses a single `AVFormatContext` with `empty_moov+frag_custom+dash` movflags
- Init segment stored separately with generation counter; AVIO swapped per segment
- Minimum 1-second segment duration enforced (Tapo cameras send keyframes every ~50-100ms)
- Audio uses independent DTS normalization (`_InitialAudioDTS`) with `stream_index=1`
- Backlog of ≥15 segments maintained for client catch-up
- `av_write_frame` does NOT take ownership of packet data — always `av_packet_unref` after. `av_interleaved_write_frame` DOES take ownership.
