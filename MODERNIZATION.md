# Witness Modernization Plan

This document outlines the plan to modernize the Witness surveillance system — updating the tech stack, adding cross-platform support (Linux/Mac), and introducing local ML-based object recognition.

## Current State

| Component | Current | Status |
|-----------|---------|--------|
| HTTP Server | Crow (replaced CppREST SDK) | ✅ Cross-platform, WebSocket-capable |
| TLS | OpenSSL via Crow (self-signed, Let's Encrypt, manual) | ✅ Auto-renewal, cert hot-reload |
| Setup | Built-in web wizard + CLI (`/setup`, `/websetup`) | ✅ Replaced 166MB .NET Installer |
| Security | CSP, HSTS, secure cookies, input sanitization | ✅ Full security headers middleware |
| Web Frontend | Knockout.js 3.4.2 | ❌ Unmaintained since 2019 |
| CSS Framework | Bootstrap 3.3.5 | ❌ EOL |
| jQuery | 2.1.4 | ⚠️ Outdated (current: 3.7+) |
| OpenCV | 4.10.0 (vcpkg) | ✅ Updated from 4.0.0-pre |
| Object Recognition | Azure Vision (cloud, disabled) + Haar cascades (local, disabled) | ⚠️ Cloud-dependent, no local ML |
| Build System | CMake + vcpkg | ✅ Migrated from .vcxproj |
| Strings | `std::string` (UTF-8) everywhere | ✅ Fully migrated from `string_t`/TCHAR |
| Platform | Windows only (Service lifecycle) | ⚠️ Code is cross-platform except service/daemon |
| Video Pipeline | FFmpeg 7.1, custom C++ pipeline | ✅ Solid, cross-platform code |
| Password Storage | libsodium argon2 | ✅ Modern and secure |
| Threading | std::thread / std::mutex | ✅ Already portable |
| Database | SQLite3 | ✅ Cross-platform |

### What's Good (Keep)

- **C++ video pipeline** (WitnessCamera) — high performance, well-isolated DLL boundary, FFmpeg-based. Nearly cross-platform already (just `Sleep()` and string types to fix).
- **Filter architecture** — plugin-based `IRecordFilter` chain with `OnSuccess()`/`OnFailure()` callbacks. New detection backends slot in with no pipeline changes.
- **HLS live streaming** — low-latency LL-HLS with fMP4 partials, battle-tested client-side watchdog.
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
├── SecurityHeadersMiddleware   ✅      Web Frontend:
├── TLS (OpenSSL)               ✅      ├── Vue 3 (Composition API)
├── Web Setup Wizard            ✅      ├── Bootstrap 5
└── vcpkg (all deps)            ✅      ├── fetch API (no jQuery)
                                        └── PWA + Web Push notifications
WitnessCamera.dll               ✅
├── std::this_thread::sleep_for ✅      Object Recognition:
├── std::string                 ✅      ├── ONNX Runtime (local detection)
└── Cross-platform pipeline     ✅      └── YOLOv8 default model

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
    "onnxruntime"      // Pending — Phase 4 (local ML)
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

### Phase 4: Local Object Recognition

- [ ] **Add ONNX Runtime** as vcpkg dependency
  - Single inference API with multiple hardware backends ("execution providers"):
    - CPU (default, works everywhere)
    - CUDA (NVIDIA GPUs)
    - DirectML (Windows, any DirectX 12 GPU)
    - CoreML (macOS)
    - OpenVINO (Intel GPUs/NPUs)
- [ ] **Create `ONNXDetectionFilter`** implementing `IRecordFilter`
  - Input: `cv::Mat` from existing `GetOrDecodeFrame()` lazy decode
  - Output: `ClassificationResult` with `RegionOfInterest` bounding boxes (existing structures)
  - Configurable confidence threshold and class label mapping
- [ ] **Configuration:**
  ```json
  {
    "recognition": {
      "backend": "onnx",
      "model_path": "models/yolov8n.onnx",
      "execution_provider": "cuda",
      "confidence_threshold": 0.5,
      "classes": ["person", "car", "dog", "cat"]
    }
  }
  ```
  Backend options: `azure`, `onnx`, `cascade`, `none`
- [ ] **Wire into filter chain** in `CameraWorker::WorkerInit()`
  - Replaces current Azure Vision filter position
  - Can coexist — config selects which backend to use per camera
- [ ] **Ship default model** — YOLO26n (~6MB, ~39ms/frame on CPU, NMS-free)
- [ ] Optionally keep Azure Vision as alternative cloud backend

### Phase 5: Notifications & Integrations

- [ ] **Web Push via VAPID** — libsodium for signing (already a dependency, cross-platform)
- [ ] **Notification triggers** from `ONNXDetectionFilter` results via existing `MessageBus`
- [ ] **MQTT event publishing** — publish detection events, camera status, and clip creation to MQTT topics. Enables Home Assistant, Node-RED, and other automation platform integration without tight coupling
- [ ] **Webhook support** — configurable HTTP POST callbacks on events (detection, camera offline, clip saved) for custom integrations
- [ ] Remove Azure Vision code if ONNX fully replaces it

### Phase 6: Feature Enhancements

- [ ] **Camera setup UI** — finish the new camera creation/editing interface so cameras can be fully configured from the web UI without manual DB edits
- [ ] **Zone/mask editing UI** — draw regions of interest and ignore zones per camera in the web UI. Reduces false positives and allows focusing detection on specific areas (e.g. driveway, not the street)
- [ ] **Activity timeline** — visual timeline view showing detected activity across cameras, making it easy to scrub through events at a glance
- [ ] **Clip date filter** — date picker / date range filter for the clips view, so users can quickly find clips from a specific time period
- [ ] **Tag search + timeline icons** — searchable tags on detected events (person, car, animal, etc.) with corresponding icons shown on the activity timeline for quick visual scanning
- [ ] **Clip interestingness scoring** — compare clip preview images against baseline frames captured at the start of each clip (during lead-in period). Calculate a visual difference score to rank clips by "interestingness" and highlight what changed in the frame. *(Design still evolving — needs further ideation on baseline selection, diff algorithm, and UI presentation)*
- [ ] **Clip export/download** — download individual clips or bulk-export date ranges as MP4/ZIP from the web UI
- [ ] **Viewer role** — non-admin users who can view live streams and clips but cannot configure cameras or server settings
- [ ] **24/7 continuous recording** — optional always-on recording mode alongside event-driven clips, with configurable retention policies and tiered storage (hot/cold)

### Phase 7: Web Frontend Modernization

- [ ] **Replace Knockout.js with Vue 3** (Composition API + single-file components)
  - Vite build toolchain (replaces manual script includes)
  - Vue Router for client-side navigation
  - Pinia for state management (replaces global ViewModel)
- [ ] **Replace Bootstrap 3 with Bootstrap 5**
  - No jQuery dependency
  - Modern responsive grid
  - CSS custom properties for theming
- [ ] **Drop jQuery** — use native `fetch` API for HTTP, `querySelector` for DOM
- [ ] **Port HLS client architecture to Vue**
  - StreamDiagnostics system, poll-based watchdog, exponential backoff
  - Preserve the "never seek from outside HLS.js" rule
  - Fullscreen camera view as dedicated Vue route
- [ ] **Internationalization (i18n)** — multi-language support via Vue I18n. Extract all user-facing strings to locale files. Natural fit after Vue migration since Vue I18n integrates cleanly with Composition API
- [ ] **Docker deployment** — Dockerfile + docker-compose.yml for containerized deployment. Depends on Linux support (Phase 1 service abstraction)

### Phase 8: Mobile & PWA

- [ ] **Add PWA support** — manifest + service worker for installable web app
- [ ] **Mobile-responsive UI** — ensure all views work well on phone/tablet screens
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

## Notes

- **WitnessCamera DLL boundary is a strength** — the video pipeline is already isolated. Cross-platform means building as `.so` (Linux) / `.dylib` (Mac) instead of `.dll`, but the C++ source is clean.
- **OpenCV and FFmpeg are natively cross-platform** — vcpkg packages work everywhere automatically.
- **Systemd integration on Linux is straightforward** — a `.service` file plus `sigaction()` signal handling replaces the entire Windows SCM layer. This is the main remaining blocker for Linux.
- **The filter architecture is the best part of the codebase for extensibility** — ONNX detection is genuinely a "write one class" task because the pipeline, frame decode, and result structures already exist.
- **Phase ordering is intentional** — each phase is independently useful and shippable. Phases 1-3 are complete. Phase 4 adds local ML. Phase 5 adds integrations. Phase 6 adds user-facing features. Phase 7 modernizes the frontend. Phase 8 adds mobile/PWA.
- **JSON: Using Crow's built-in JSON** — `crow::json::wvalue`/`rvalue` handles all needs. No external JSON library required. Auto-sets `Content-Type: application/json` on responses.
