# Witness Modernization Plan

This document outlines the plan to modernize the Witness surveillance system — updating the tech stack, adding cross-platform support (Linux/Mac), and introducing local ML-based object recognition.

## Current State

| Component | Current | Status |
|-----------|---------|--------|
| HTTP Server | Crow (replaced CppREST SDK) | ✅ Cross-platform, WebSocket-capable |
| TLS | OpenSSL via Crow (self-signed, Let's Encrypt, manual) | ✅ Auto-renewal, cert hot-reload |
| Setup | Built-in web wizard + CLI (`/setup`, `/websetup`) | ✅ Replaced 166MB .NET Installer |
| Security | CSP, HSTS, secure cookies, input sanitization | ✅ Full security headers middleware |
| Web Frontend | Vue 3 + Bootstrap 5 (new), Knockout.js (legacy) | 🔄 New UI in progress |
| CSS Framework | Bootstrap 5 (new), Bootstrap 3.3.5 (legacy) | 🔄 New UI in progress |
| jQuery | Removed from new UI; 2.1.4 in legacy | 🔄 Legacy only |
| Live Streaming | HLS + LL-HLS (per-camera), auto-refresh on deploy | ✅ Client watchdog, gap-skip, latency cap |
| Continuous Recording | 24/7 MP4 segments, per-camera toggle, retention | ✅ Backend + frontend |
| DVR Playback | Multi-camera player, thumbnail scrubbing, keyboard controls | ✅ Full playback + scrub preview |
| OpenCV | 4.10.0 (vcpkg) | ✅ Updated from 4.0.0-pre |
| Object Recognition | ONNX Runtime + YOLO26n (local), background reprocessor | ✅ Local ML, GPU optional |
| Build System | CMake + vcpkg | ✅ Migrated from .vcxproj |
| Strings | `std::string` (UTF-8) everywhere | ✅ Fully migrated from `string_t`/TCHAR |
| Platform | Windows only (Service lifecycle) | ⚠️ Code is cross-platform except service/daemon |
| Video Pipeline | FFmpeg 7.1, custom C++ pipeline | ✅ Solid, cross-platform code |
| Password Storage | libsodium argon2 | ✅ Modern and secure |
| Threading | std::thread / std::mutex | ✅ Already portable |
| Database | SQLite3 with relational tag system | ✅ Cross-platform |
| Tags | Relational Tag/ClipTag tables with emoji icons | ✅ Migrated from semicolon-delimited strings |
| Real-time Events | WebSocket push (clip created/reprocessed) | ✅ Live updates |

### What's Good (Keep)

- **C++ video pipeline** (WitnessCamera) — high performance, well-isolated DLL boundary, FFmpeg-based. Nearly cross-platform already (just `Sleep()` and string types to fix).
- **Filter architecture** — plugin-based `IRecordFilter` chain with `OnSuccess()`/`OnFailure()` callbacks. New detection backends slot in with no pipeline changes.
- **HLS live streaming** — standard HLS with fMP4 segments, LL-HLS partial segments optional per camera, client-side watchdog with gap-skip debouncing, latency cap, beyondBuffer recovery, and exponential backoff. Auto-refresh on deploy via build hash mechanism.
- **SQLite database** — portable, zero-config, parameterized queries (no SQL injection risk).
- **libsodium auth** — argon2 password hashing, cryptographically random sessions.

### What's Blocking Linux/Mac

| Blocker | Severity | Status |
|---------|----------|--------|
| Windows Service (SCM) architecture | Major | Pending — extract to `platform/` |
| ~~CppREST SDK — archived, limited Linux support~~ | ~~Major~~ | ✅ Done — replaced with Crow |
| ~~`string_t` / `_T()` / `tcout` / TCHAR everywhere~~ | ~~Major~~ | ✅ Done — `std::string` throughout |
| ~~`Sleep()` instead of `std::this_thread::sleep_for()`~~ | ~~Medium~~ | ✅ Done |
| ~~Backslash path separators~~ | ~~Medium~~ | ✅ Done — `std::filesystem::path` |
| `%ProgramData%` / `_tgetenv_s()` paths | Medium | Pending |
| Windows console APIs | Low | Already has `#ifdef` |
| ~~Pre-built Windows DLLs~~ | ~~Build~~ | ✅ Done — all via vcpkg |
| ~~No CMake — only `.vcxproj` build files~~ | ~~Build~~ | ✅ Done — CMake + vcpkg |

### What Exists for Object Recognition

The filter architecture is already plugin-based and ready for new backends:

- `IRecordFilter` interface with `OnSuccess()` / `OnFailure()` callback chain
- `PersonRecognitionFilter` exists (Haar cascades) but is **commented out** in `CameraWorker.cpp`
- Azure Vision filter sends frames to cloud API via HTTP
- Frames available as `cv::Mat` (BGR24) via lazy decode from `AVFrame`
- Results use `ClassificationResult` with `RegionOfInterest` bounding boxes + classification enums
- Filter chain is wired in `CameraWorker::WorkerInit()` — configurable per-camera

### Known Security Issues ~~to Fix During Migration~~ — FIXED ✅

All security issues identified during the initial audit have been resolved:

| Issue | Status | Resolution |
|-------|--------|------------|
| ~~CORS `Access-Control-Allow-Origin: *` on stream endpoints~~ | ✅ Fixed | Removed — auth required on all endpoints |
| ~~Session cookie missing `Secure; SameSite` flags~~ | ✅ Fixed | `HttpOnly; Secure; SameSite=Strict` |
| ~~`#if !_DEBUG` auth bypass on stream endpoint~~ | ✅ Fixed | Removed — all endpoints require auth |
| ~~No CSP header~~ | ✅ Fixed | `SecurityHeadersMiddleware` on all responses |
| ~~Inline `onclick` handlers in templates~~ | ✅ Fixed | Moved to script blocks |

---

## Target Architecture

```
Completed:                              Remaining:
─────────────────────────────           ─────────────────────────
WitnessServer.exe                       witness-server (Linux/Mac)
├── Crow (HTTP + WebSocket)     ✅      ├── systemd / launchd service
├── Route handlers              ✅      └── Platform path abstraction
├── std::string (UTF-8)         ✅
├── SecurityHeadersMiddleware   ✅      Web Frontend (remaining):
├── TLS (OpenSSL)               ✅      ├── Port remaining legacy pages
├── Web Setup Wizard            ✅      ├── PWA + Web Push notifications
├── WebSocket events            ✅      └── i18n
└── vcpkg (all deps)            ✅
                                        
WitnessCamera.dll               ✅
├── std::this_thread::sleep_for ✅
├── std::string                 ✅
└── Cross-platform pipeline     ✅

Object Recognition:             ✅
├── ONNX Runtime (local)        ✅
├── YOLO26n default model       ✅
├── CUDA GPU acceleration       ✅
└── Background reprocessor      ✅

Web Frontend (new Vue 3):       🔄
├── Vue 3 + Composition API     ✅
├── Vite build toolchain        ✅
├── Vue Router                  ✅
├── Pinia state management      ✅
├── Bootstrap 5                 ✅
├── HLS.js composable           ✅
├── Auto-refresh (build hash)   ✅
├── Dashboard (live cameras)    ✅
├── Clips browser + filters     ✅
├── Activity timeline + zoom    ✅
├── DVR playback + thumbnails   ✅
├── Tag management admin        ✅
├── Camera management admin     ✅
├── Detection settings admin    ✅
└── User management admin       ✅

Tag System:                     ✅
├── Relational Tag/ClipTag DB   ✅
├── Tag CRUD API                ✅
├── Emoji icons per tag         ✅
├── Tag grouping (aliases)      ✅
├── Per-camera tag exclusions   ✅
└── Tag-based clip filtering    ✅

Build:
├── CMakeLists.txt (root)       ✅
│   ├── WitnessCamera/          ✅
│   └── WitnessServer/          ✅
├── vcpkg.json (all deps)       ✅
└── GitHub Actions CI                   Pending
```

### Target vcpkg Dependencies

```json
{
  "dependencies": [
    "crow",            // ✅ HTTP server
    "ffmpeg",          // ✅ Video pipeline
    "opencv4",         // ✅ Image processing
    "libsodium",       // ✅ Auth + crypto
    "openssl",         // ✅ TLS
    "sqlite3",         // ✅ Database
    "onnxruntime-gpu"  // ✅ Local ML (CPU + CUDA)
  ]
}
```

---

## Implementation Phases

### Phase 1: Cross-Platform Foundation ✅ COMPLETE

Made the codebase buildable on Linux without changing functionality.

- [x] **Replace `Sleep()` with `std::this_thread::sleep_for()`**
  - 6 call sites across 4 files (`CameraWorker.cpp`, `WatchdogWorker.cpp`, `TimerWorker.cpp`, `Main.cpp`)
  - Removed `#include <windows.h>` from files that only needed it for `Sleep()`
- [x] **Portable path handling**
  - Replaced `CreateDirectory`/`CreateDirectoryW` → `std::filesystem::create_directories()`
  - Replaced backslash path joins → `std::filesystem::path /` operator
  - Replaced `std::tr2::sys::path` (deprecated) → `std::filesystem::path`
- [x] **Introduce `StringT`/`CharT`/`StringStreamT` intermediary types**
  - Defined in `Common.h` as aliases (later migrated to `std::string`/`char` in Phase 2)
- [x] **CMake migration** — replaced `.vcxproj`/`.sln` with CMake + vcpkg
  - Created `CMakeLists.txt` (root), `WitnessCamera/CMakeLists.txt`, `WitnessServer/CMakeLists.txt`
  - Created `CMakePresets.json` with vcpkg toolchain integration
  - All dependencies via vcpkg: ffmpeg, opencv4, libsodium, sqlite3, crow, openssl
  - Updated OpenCV from 4.0.0-pre to 4.10+ (fixed deprecated C API usage)
- [x] **Replace `string_t` / `_T()` / TCHAR with `std::string` (UTF-8)** — completed during Phase 2
  - `StringT` → `std::string`, `CharT` → `char`, `_T()` → no-op
  - `std::tcout/tcerr/tcin` → `std::cout/cerr/cin`
  - SQLite wrapper and Database.cpp converted to narrow strings
  - WitnessCamera `MotionVectorFilter` API converted to `const char*`
- [ ] **Abstract service/daemon lifecycle**
  - Extract Windows SCM code from `Main.cpp` into `platform/windows/Service.cpp`
  - Create `platform/linux/Daemon.cpp` using systemd (`.service` file + signal handling)
  - Common `main()` entry point calls platform-specific init
  - Signal handling: `ConsoleHandlerRoutine` → `sigaction(SIGTERM/SIGINT)`
- [ ] **Cross-platform CI** — GitHub Actions matrix build (Windows MSVC, Linux GCC/Clang)

#### Building with CMake

```bash
# Configure (first time — vcpkg will build dependencies, ~30 min)
cmake --preset default

# Build
cmake --build build --config Release

# Output
#   build/WitnessCamera/Release/WitnessCamera.dll
#   build/WitnessServer/Release/WitnessServer.exe
```

Requires vcpkg installed and `VCPKG_ROOT` environment variable set (or edit `CMakePresets.json`).

### Phase 2: Replace HTTP Server ✅ COMPLETE

CppREST SDK was the primary Linux blocker. Fully replaced with Crow.

- [x] **Replace CppREST SDK with Crow**
  - Cross-platform, header-only HTTP server via vcpkg
  - Uses `std::string` natively — completed string migration
  - JSON via `crow::json` (built-in, no external JSON library needed)
- [x] Migrate `WitnessListener` → `CrowListener` (Crow app with `SecurityHeadersMiddleware`)
- [x] Migrate all route handlers → Crow route handlers (split into per-domain files):
  - `CrowRoutes_Camera.cpp` — Preview, enum, record, create, delete, set_groups, reset_stats
  - `CrowRoutes_Stream.cpp` — HLS playlist and segment serving
  - `CrowRoutes_Auth.cpp` — Login, logout, profile, user management
  - `CrowRoutes_Clip.cpp` — Thumbnails, video, enum, toggle save, delete
  - `CrowRoutes_Group.cpp` — CRUD group operations
  - `CrowRoutes_Debug.cpp` — Debug value enum, set, reset
  - `CrowRoutes_Setup.cpp` — Admin-only reconfiguration
- [x] Remove CppREST SDK entirely (deleted from vcpkg.json, all old handlers removed)
- [x] Remove cloud HTTP clients (AndroidNotify, AzureEndpoint — disabled, not replaced)
- [x] **Security fixes:**
  - CSP + security headers via `SecurityHeadersMiddleware` (`after_handle`)
  - `X-Content-Type-Options`, `X-Frame-Options`, `HSTS`, `Referrer-Policy`
  - Session cookies: `HttpOnly; Secure; SameSite=Strict`
  - Removed CORS wildcard and `#if !_DEBUG` auth bypass
  - TLS support via Crow + OpenSSL (self-signed, Let's Encrypt, manual certs)
  - Background cert monitor with 12-hour mtime check + `/debug/reload_tls` endpoint

### Phase 3: Built-in Web Setup Wizard ✅ COMPLETE

Replaced the 166MB .NET Installer with a built-in web setup wizard.

- [x] **First-run setup mode** — auto-detects no admin user, starts localhost-only HTTP wizard on random ephemeral port (49152-65000), auto-opens browser on Windows
- [x] **Headless/CLI setup** — `/setup` for interactive console wizard, `/setup --json <path>` for scripted deployments
- [x] **Web wizard UI** — 5-step wizard (Welcome → Server Settings → TLS → Admin Account → Review & Apply)
- [x] **Elevation helper** — `/apply-config <path>` for privileged actions (service install, firewall) via UAC on Windows
- [x] **Reconfiguration** — `/setup` endpoint on production server (admin-only), pre-populated with current settings
- [x] **`/websetup` CLI flag** — forces web wizard even when admin exists
- [x] **Password reset** — reconfigure mode allows optional admin password reset
- [x] **Security hardening** — hostname sanitization for openssl, unpredictable temp file names, JSON injection prevention
- [x] **Installer deprecated** — moved to optional CMake target, README updated

### Phase 4: Local Object Recognition ✅ COMPLETE

- [x] **Add ONNX Runtime** as vcpkg dependency (`onnxruntime-gpu`)
  - CPU (default) and CUDA (NVIDIA GPU) execution providers
- [x] **Create `ONNXDetectionFilter`** implementing `RecordFilterBase`
  - Input: `cv::Mat` from existing lazy decode
  - Output: `DetectionResult` with class ID, confidence, and class name
  - Configurable confidence threshold and max FPS throttle
  - Standalone `DetectFrame()` method for clip reprocessor
- [x] **Configuration via DB settings:**
  - `detection_backend` — `onnx` or empty (disabled)
  - `detection_provider` — `cpu` or `gpu`
  - `detection_confidence` — 0.1 to 1.0
  - `detection_max_fps` — frames per second limit per camera
  - `cudnn_path` — optional cuDNN install root (auto-scanned if empty)
- [x] **Wire into filter chain** in `CameraWorker::WorkerInit()`
- [x] **Ship default model** — YOLO26n (`models/yolo26n.onnx`)
- [x] **Background clip reprocessor** (`ClipReprocessWorker`)
  - Processes existing clips with detection, updates tags in DB
  - First frame baseline filtering (filters permanent scene objects)
  - Tag separator `;` matches Recording.cpp convention
  - `DetectionVersion` tracking (-1=retag priority, 0=unprocessed, 1=current)
  - Batched processing with `ORDER BY DetectionVersion ASC, Timestamp DESC`
- [x] **Clip re-tag** — `POST /clip/retag/{id}` endpoint, UI button on clips page
- [x] **CUDA GPU acceleration:**
  - cuDNN auto-discovery via `LoadLibraryW` pre-loading from common install paths
  - User-configurable `cudnn_path` setting for custom cuDNN locations
  - Static CUDA availability caching (tested once, cached for all cameras)
  - Conservative memory: `OrtCudnnConvAlgoSearchDefault` + `arena_extend_strategy=1`
  - **CUDA probe process** — `/test-cuda` CLI command tests CUDA in child process, safe against cuDNN `__fastfail` crashes
  - "Test GPU" button in admin panel and setup wizard
  - ONNX provider DLLs copied via CMake POST_BUILD step
- [x] **Admin detection panel** — detection settings + clip cleanup toggle in admin portal
- [x] **Setup wizard detection** — object detection section with GPU requirements and links
- [x] **Structured logging** — `spdlog`-based `LOG_INFO/WARNING/ERROR/DEBUG` via `Log.h`
- [x] **SQLite busy timeout** — `sqlite3_busy_timeout(db, 5000)` prevents write contention
- [x] **SQLite error diagnostics** — `AssertQuery` macro prefixes errors with `[QueryName]`, error callback uses `LOG_ERROR` instead of `LOG_INFO`, making failed queries immediately identifiable in logs
- [x] **Clip cleanup setting** — `clip_cleanup_enabled` / `clip_retention_days` DB settings, configurable in admin panel, defaults to disabled
- [x] **Let's Encrypt certbot** — setup wizard runs certbot with UAC elevation, auto-installs via winget if needed, warns about local access requirement
- [ ] **Improve night/IR detection accuracy**
  - Standard YOLO models are trained on COCO (daytime RGB) and perform poorly on grayscale IR-illuminated CCTV footage
  - **Option A: Preprocessing** — Apply CLAHE (Contrast Limited Adaptive Histogram Equalization) to IR frames before inference
  - **Option B: Fine-tune on IR data** — Use the [FLIR ADAS Thermal Dataset](https://www.flir.com/oem/adas/adas-dataset-form/)
  - **Option C: Dedicated IR model** — Purpose-built models like CRT-YOLO or MDCFVit-YOLO
- [ ] Optionally keep Azure Vision as alternative cloud backend

### Phase 5: Notifications & Integrations

- [ ] **Web Push via VAPID** — libsodium for signing (already a dependency, cross-platform)
- [ ] **Notification triggers** from `ONNXDetectionFilter` results via existing `MessageBus`
- [ ] **MQTT event publishing** — publish detection events, camera status, and clip creation to MQTT topics. Enables Home Assistant, Node-RED, and other automation platform integration without tight coupling
- [ ] **Webhook support** — configurable HTTP POST callbacks on events (detection, camera offline, clip saved) for custom integrations
- [ ] Remove Azure Vision code if ONNX fully replaces it

### Phase 6: Feature Enhancements 🔄 IN PROGRESS

- [ ] **Camera setup UI** — finish the new camera creation/editing interface so cameras can be fully configured from the web UI without manual DB edits
- [ ] **Zone/mask editing UI** — draw regions of interest and ignore zones per camera in the web UI. Reduces false positives and allows focusing detection on specific areas (e.g. driveway, not the street)
- [x] **Activity timeline** — visual timeline showing detected activity across cameras with server-side aggregated events, adaptive time bucketing (5min → 1week based on range), camera color-coding, emoji tag markers, drag-to-select time ranges, calendar date picker, and clip retention cutoff marker
- [x] **Timeline zoom** — drag-to-select zooms into sub-range with zoom history stack, ✕ button and double-click to zoom back out, arrows zoom out first when zoomed
- [x] **Clip date filter** — unified time presets (Today/Yesterday/This Week/Last Week/This Month/Last Month/Older/Custom range) integrated into the timeline nav bar, driving both timeline and clip grid. Atomic `setTimeRange()` prevents race conditions from split filter updates.
- [x] **Tag search + timeline icons** — searchable tags with emoji icons shown on the activity timeline per event, tag-based filtering in the clip browser sidebar
- [x] **24/7 continuous recording** — per-camera toggle, 60s MP4 segments with configurable retention, real-time WebSocket `dvr:segment` events, coverage API for timeline
- [x] **DVR playback** — multi-camera synchronized player with HLS playlist generation from continuous segments, segment-by-segment seek, A/B video slot swapping for gapless playback
- [x] **DVR thumbnail scrubbing** — server-side on-demand thumbnail endpoint (`GET /dvr/thumbnail/<cameraId>/<timestamp>`) with FFmpeg keyframe decode, OpenCV JPEG encode at 300px/q70, LRU cache (50 entries). Timeline hover shows all cameras with coverage at that timestamp. DVR player seek bar shows scrub preview on hover.
- [x] **DVR controls polish** — download current segment button, keyboard shortcuts (Space=play/pause, ←/→=±5s, 1/2/4/8=playback rate), cross-segment seeking
- [ ] **Clip interestingness scoring** — compare clip preview images against baseline frames captured at the start of each clip (during lead-in period). Calculate a visual difference score to rank clips by "interestingness" and highlight what changed in the frame. *(Design still evolving — needs further ideation on baseline selection, diff algorithm, and UI presentation)*
- [ ] **Clip export/download** — download individual clips or bulk-export date ranges as MP4/ZIP from the web UI
- [ ] **Viewer role** — non-admin users who can view live streams and clips but cannot configure cameras or server settings
- [ ] **DVR quota management** — `continuous_recording_quota_gb` and `clip_quota_gb` settings, disk space safety pruning, crash recovery for unfinalized segments
- [ ] **DVR pins/bookmarks** — mark points on the timeline with labels, pin CRUD API, export video between pins
- [ ] **WebSocket disconnect overlay** — full-screen overlay with disconnected logo when WebSocket connection is lost

### Phase 7: Web Frontend Modernization 🔄 IN PROGRESS

- [x] **Replace Knockout.js with Vue 3** (Composition API + single-file components)
  - Vite build toolchain with dev proxy to backend
  - Vue Router for client-side navigation (hash mode)
  - Pinia for state management (filters, clips, tags, cameras)
  - Served at `/witness2/` alongside legacy UI at `/witness/`
- [x] **Replace Bootstrap 3 with Bootstrap 5**
  - Dark theme throughout
  - No jQuery dependency
  - Modern responsive grid
- [x] **Drop jQuery** — all new code uses native `fetch` API and Vue reactivity
- [x] **Port HLS client architecture to Vue**
  - `useHls` composable wrapping HLS.js lifecycle
  - Standard HLS and LL-HLS modes (per-camera toggle)
  - Poll-based watchdog: stuck detection (readyState ≤ 2 + frags stopped), stall detection, exponential backoff
  - Gap-skip with 1s debounce, beyondBuffer recovery with 5s cooldown and 2s tolerance
  - Latency cap using `hls.latency` (seeks to live edge when >8s behind)
  - StreamDiagnostics preserved (`window._witnessDiag`, `window._witnessDumpAll()`)
  - Fatal network errors trigger immediate restart; non-fatal stalls left to HLS.js internal recovery
- [x] **Auto-refresh on deploy** — Vite plugin generates random build hash, server broadcasts via WebSocket, client auto-reloads on mismatch with sessionStorage reload tracking
- [x] **Vue pages implemented:**
  - Dashboard — live camera grid with HLS streams, LL-HLS per-camera toggle
  - Clips browser — paginated grid with filters, tag display, player modal
  - Admin — camera management, user management, tag management, detection settings, debug values
  - Login page
- [x] **Activity timeline** — aggregated event timeline with emoji markers, drag-to-select with zoom history, calendar picker, time presets, retention cutoff line, DVR coverage bars with multi-camera thumbnail hover preview
- [x] **DVR playback** — multi-camera synchronized player, seek bar scrub preview, download segment, keyboard shortcuts, A/B video slot swapping
- [x] **Tag system UI** — filter sidebar with tag toggles (grouped by display name), tag admin with emoji/display editing, per-camera exclusions
- [x] **WebSocket integration** — real-time clip creation and reprocessing events update the clip grid live
- [ ] **Port remaining legacy pages** — some legacy Knockout.js pages may still be in use
- [ ] **Internationalization (i18n)** — multi-language support via Vue I18n. Extract all user-facing strings to locale files
- [ ] **Docker deployment** — Dockerfile + docker-compose.yml for containerized deployment. Depends on Linux support (Phase 1 service abstraction)

### Phase 8: Mobile & PWA

- [ ] **Add PWA support** — manifest + service worker for installable web app
- [ ] **Mobile-responsive UI** — ensure all views work well on phone/tablet screens
  - ⚠️ Known issue: current web UI is unusable on mobile — menu/sidebar not accessible without switching to desktop mode
  - Need hamburger menu, touch-friendly controls, responsive layout breakpoints
- [ ] **Evaluate Flutter/Android projects** — PWA may replace native apps entirely

---

## Key Technical Decisions

### String Migration Strategy ✅ COMPLETE

Completed incrementally across Phase 1 and 2:

1. Phase 1: Introduced `StringT`/`CharT` typedefs as intermediary
2. Phase 2: Changed typedefs to `std::string`/`char`, removed `_T()` macros, converted SQLite layer and all APIs to narrow strings

### Why Crow Over Alternatives ✅ CHOSEN

| | Crow | Drogon | cpp-httplib | Beast |
|---|---|---|---|---|
| Cross-platform | ✅ | ✅ | ✅ | ✅ |
| Header-only | ✅ | ❌ | ✅ | ✅ |
| WebSocket | ✅ | ✅ | ❌ | ✅ |

### Low-Latency Streaming 🔄 PARTIALLY WORKING

**Current state:** Standard HLS (~4-6s latency) is the default. LL-HLS (~1-2s latency) is available as a per-camera toggle but has known reliability issues under extended operation.

**LL-HLS implementation (done):**
- Server: `#EXT-X-PART` partial segments, `CAN-BLOCK-RELOAD`, `PART-HOLD-BACK`, per-camera toggle in admin UI
- Client: HLS.js LL-HLS mode with `liveSyncPosition` tracking, `hls.latency` measurement
- Latency overlay in debug diagnostics

**LL-HLS fixes applied:**
- **EXTINF duration mismatch fix:** `#EXTINF` now uses accumulated `AVPacket.duration` instead of DTS span — eliminates the 0.002s/segment divergence that caused 47s drift over 24h (cam9 stuck loop root cause)
- **Segment staleness expiry:** `GetSegments()` prunes individual segments older than 10s — prevents stale backlog replay loops
- **503 on empty playlist:** returns HTTP 503 instead of unparseable empty M3U8 when all segments are stale — HLS.js retries gracefully instead of entering manifestParsingError loop
- **Gap-skip debounce:** 1s debounce window prevents false positive seeks during normal segment transitions
- **beyondBuffer tolerance:** only triggers when playhead is >2s past buffer end (was 0s, causing 3000+ false positives in LL-HLS where playhead naturally sits at buffer edge)
- **beyondBuffer cooldown:** 5s cooldown after beyondBuffer seeks prevents seek loops in LL-HLS
- **Latency cap:** seeks to live edge when `hls.latency > 8s` (uses HLS.js built-in latency, not broken `liveSyncPosition - currentTime` formula)
- **Auto-refresh on deploy:** build hash mechanism — Vite generates random hash per build, server broadcasts via WebSocket, client auto-reloads on mismatch

**Remaining LL-HLS issues:**
- cam8/cam9 still accumulate negative drift (non-standard GOP / variable frame rates) — partially mitigated by staleness expiry + latency cap
- Browser HTTP connection pool exhaustion when 7+ LL-HLS cameras poll simultaneously — all cameras lose fragments at once
- Needs extended soak testing before enabling on production

**Alternative approach (WebSocket + MSE):**

Push fMP4 fragments directly from server to client over WebSocket, appending to MSE SourceBuffer. This eliminates playlist polling overhead entirely.

Target latency: **~0.3-0.5s** (partial duration + network time).

**Server side:**
- Add binary WebSocket route: `/ws/stream/{cameraId}`
- On connect: send init segment (ftyp+moov) as first binary frame
- Subscribe to `LiveOutputStream`'s partial production (the existing `FlushPartialSegment` already produces fMP4 fragments)
- Push each partial as a binary WebSocket frame immediately
- Handle camera reconnect: send new init segment with a "reset" control message so client can recreate its SourceBuffer
- Backpressure: if the WebSocket send buffer grows too large (slow client), drop non-keyframe partials

**Client side:**
- Open WebSocket connection to `/ws/stream/{cameraId}`
- Create `MediaSource` + `SourceBuffer` (codec from init segment's moov atom, or hardcode `video/mp4; codecs="avc1.4d0029"` and update on connect)
- First binary message = init segment → append to SourceBuffer
- Subsequent binary messages = moof+mdat partials → append to SourceBuffer
- Buffer management: remove old data when buffer exceeds threshold (e.g., keep last 10s)
- Handle `SourceBuffer.updating` state — queue appends while updating
- Reconnect with exponential backoff on WebSocket close/error
- Reuse existing watchdog pattern (spinner/connection-lost indicators)
- Keep HLS.js as fallback for iOS Safari (no MSE support) or when WebSocket unavailable

**Key risks:**
- MSE `SourceBuffer.appendBuffer()` is async — need to queue fragments and process sequentially
- Browser may reject fragments if codec parameters change mid-stream (camera reconnect)
- No built-in seeking/DVR — this is live-only (DVR already uses native `<video>` with MP4s)
- Need to handle tab backgrounding (browsers throttle timers/WebSocket in background tabs)

**Alternative (simpler, higher latency): Fix LL-HLS properly**
- Compute `#EXTINF` / `#EXT-X-PART:DURATION` from DTS spans instead of accumulated packet durations
- Increase `PART-HOLD-BACK` to 2-3s (compromise between latency and reliability)
- Achievable latency: ~1.5-2s (limited by playlist polling overhead)
- Lower risk but can't match WebSocket's sub-second latency
| Routing DSL | ✅ Simple | ✅ Rich | ❌ Manual | ❌ Manual |
| vcpkg | ✅ | ✅ | ✅ | ✅ (Boost) |
| Learning curve | Low | Medium | Low | High |
| Community | Active | Active | Active | Boost ecosystem |

Crow was chosen for simplicity. Its routing DSL mapped cleanly to the existing route handlers. Built-in JSON (`crow::json`) eliminated the need for nlohmann-json. `SecurityHeadersMiddleware` provides per-response security headers via Crow's middleware system.

### Why ONNX Runtime Over Alternatives

| | ONNX Runtime | TensorRT | LibTorch | TFLite |
|---|---|---|---|---|
| Cross-platform | ✅ All | ❌ NVIDIA only | ✅ | ✅ |
| GPU vendors | All (via providers) | NVIDIA only | NVIDIA (CUDA) | Limited |
| Model format | .onnx (universal) | .engine (proprietary) | .pt | .tflite |
| Model ecosystem | All frameworks export to ONNX | Convert from ONNX | PyTorch only | TensorFlow only |
| C++ API | ✅ | ✅ | ✅ | ✅ |
| vcpkg | ✅ | ❌ | ❌ | ❌ |

ONNX Runtime's execution provider model means the same code runs on CUDA (NVIDIA), DirectML (AMD/Intel on Windows), CoreML (Mac), or CPU — just a config change. Model files (.onnx) are a universal export format supported by PyTorch, TensorFlow, and most training frameworks.

### Why Vue 3 Over Alternatives

| | Vue 3 | React | Svelte | Angular |
|---|---|---|---|---|
| Bundle size | 33KB | 42KB | 2KB (compiled) | 143KB |
| Learning curve | Low | Medium | Low | High |
| Reactivity | Built-in | External (hooks) | Compiler magic | RxJS |
| Migration from KO | Natural (similar reactivity model) | Paradigm shift | Natural | Heavy |
| TypeScript | Optional | Optional | Optional | Required |

Vue's reactivity system (`ref`, `computed`, `watch`) maps almost 1:1 to Knockout's observables/computeds/subscriptions, making migration more intuitive than React's hooks paradigm.

---

## Future Ideas

These are longer-term ideas that don't fit neatly into existing phases — they may become their own phases or fold into existing ones as designs mature.

### Clip Quick Filters ✅ COMPLETE

Implemented as part of the Vue 3 clip browser:
- Time presets in timeline nav: **Today**, **Yesterday**, **This Week**, **Last Week**, **This Month**, **Last Month**, **Older**, **Custom Range** (date picker popup)
- Filter sidebar with: reviewed/saved/unsaved status, lighting mode (day/night), minimum duration slider, tag toggles (grouped by display name)
- Drag-to-select time range on the activity timeline with zoom history stack (zoom out via ✕ button, double-click, or arrow keys)
- All filters stored in Pinia filter store and applied via API query params

### Tag Database Redesign ✅ COMPLETE

The tag system has been fully redesigned from semicolon-delimited strings to a proper relational model:

- **`Tag` table** — `TagUID` (auto-increment), `Name` (unique), `Display` (user label), `Icon` (emoji), `SortOrder`, `Hidden` (global exclusion), `ClipCount` (cached)
- **`ClipTag` junction table** — `ClipUID + TagUID` composite primary key with foreign keys
- **`CameraTagExclusion` table** — per-camera tag hiding
- Startup migration parses existing `Clip.Tags` column → populates `Tag` + `ClipTag` tables
- Built-in emoji icon map for YOLO classes (person→🧑, car→🚗, cat→🐱, dog→🐕, etc.)
- Recording.cpp and ClipReprocessWorker.cpp use INSERT OR IGNORE for new tags
- Legacy `Tags` column still written for backward compatibility
- Tag admin UI: rename, change emoji, hide/unhide, group by display name
- Tag grouping: tags with same display name filter together and show as one entry
- Per-camera tag exclusion management in admin UI

### Detection Focus & Artefact Grid

Extract highlighted objects (faces, people, vehicles, animals) from detection frames into individual cropped images ("artefacts"), stored alongside clips. These feed into two views:

**Per-clip collage:**
- During recording, crop bounding boxes from detection results and save as small JPEGs
- Select the best/most diverse crops (highest confidence, different classes, different timestamps)
- Compose into a grid collage image stored alongside the clip thumbnail
- On hover over a clip card, animate through the sequence of crops as a mini slideshow

**Artefact grid (new view):**
- Standalone page showing all detected object crops across all clips, filterable by tag/time/camera
- Masonry or uniform grid layout — users quickly scan dozens of faces, people, vehicles at a glance
- Each artefact links back to its source clip at the exact timestamp
- Filter by detection type: "Show me all people", "Show me all vehicles"
- Time-based browsing: artefacts grouped by hour/day, scrollable
- Search/sort by confidence score, size, camera
- Pairs naturally with face recognition (Phase: Face Detection) — clicking a face artefact could show all matches
- Useful for forensics: "who was on the property today?" → scan the people artefact grid instead of watching hours of video

**Pipeline integration:**
- Runs as part of the existing detection filter chain — crops extracted when bounding boxes are already available
- Storage: `CachePath/artefacts/{ClipUID}/{timestamp}_{class}_{index}.jpg` (~5-20KB each)
- DB: `Artefact` table — `ArtefactUID, ClipUID, TagUID, Timestamp, BBox (x,y,w,h), Confidence, FilePath`
- Background reprocessor can regenerate artefacts for existing clips

### Event Bisection Search

Binary search tool for finding when something specific happened. Useful for forensic investigation (e.g., "when was the car stolen?").

- User selects a time range (e.g., "last 48 hours") and a camera
- System shows a clip from the midpoint and asks: "Has it happened yet?" → **Yes** / **No** / **Can't tell**
- Based on the answer, the range narrows by half (or shifts if "can't tell")
- Repeats until the range is small enough to identify the exact event
- Could show side-by-side "before" and "after" thumbnails at each step for quick comparison
- Display a visual range indicator showing the narrowing search window
- Typically finds the exact clip in ~10 steps even across thousands of clips

### Spatial Motion Indexing

Tag and track motion at specific locations within a camera's field of view, enabling queries like "show me everything that happened in this area."

- User draws a region of interest on a camera frame (polygon or rectangle)
- System indexes motion activity within that region across all clips for that camera
- Query: "show all clips where motion occurred in this zone" with timeline visualization
- Could use existing detection bounding boxes to determine spatial overlap
- Enables use cases like: monitoring a specific parking spot, doorway, or gate
- Could generate per-zone activity heatmaps over time

### Face Detection & Recognition

Detect and recognize faces across clips, enabling search by person and alerting on known/unknown faces.

- **Face detection:** use a lightweight face detection model (e.g. SCRFD, RetinaFace, or MediaPipe) to locate faces in detection frames
- **Face embeddings:** extract face embeddings (128/512-dim vectors) using a recognition model (e.g. ArcFace via ONNX Runtime — same infrastructure as YOLO)
- **Face clustering:** group unknown faces by similarity (cosine distance) into auto-generated "Person 1", "Person 2" clusters
- **Named faces:** user can label clusters with names — future detections auto-match
- **Integration with existing pipeline:** runs as an additional `IRecordFilter` after person detection (only process frames where a person was detected)
- **Search:** "show all clips containing [person]" — filters clip browser by face cluster
- **Alerts:** optional notification when an unknown face is detected, or when a specific known person appears
- **Privacy considerations:** face data should be deletable per-person, and face recognition should be an opt-in feature

---

## Notes

- **WitnessCamera DLL boundary is a strength** — the video pipeline is already isolated. Cross-platform means building as `.so` (Linux) / `.dylib` (Mac) instead of `.dll`, but the C++ source is clean.
- **OpenCV and FFmpeg are natively cross-platform** — vcpkg packages work everywhere automatically.
- **Systemd integration on Linux is straightforward** — a `.service` file plus `sigaction()` signal handling replaces the entire Windows SCM layer. This is the main remaining blocker for Linux.
- **The filter architecture is the best part of the codebase for extensibility** — ONNX detection is genuinely a "write one class" task because the pipeline, frame decode, and result structures already exist.
- **Phase ordering is intentional** — each phase is independently useful and shippable. Phases 1-4 are complete. Phase 5 adds integrations. Phases 6-7 are in progress (activity timeline, clip browser, tag system, Vue 3 frontend). Phase 8 adds mobile/PWA.
- **JSON: Using Crow's built-in JSON** — `crow::json::wvalue`/`rvalue` handles all needs. No external JSON library required. Auto-sets `Content-Type: application/json` on responses.
