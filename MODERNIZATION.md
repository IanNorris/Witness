# Witness Modernization Plan

This document outlines the plan to modernize the Witness surveillance system — updating the tech stack, adding cross-platform support (Linux/Mac), and introducing local ML-based object recognition.

## Current State

| Component | Current | Status |
|-----------|---------|--------|
| HTTP Server | CppREST SDK 2.9.1 | ❌ Archived/dead — primary Linux blocker |
| Web Frontend | Knockout.js 3.4.2 | ❌ Unmaintained since 2019 |
| CSS Framework | Bootstrap 3.3.5 | ❌ EOL |
| jQuery | 2.1.4 | ⚠️ Outdated (current: 3.7+) |
| OpenCV | 4.10.0 (vcpkg) | ✅ Updated from 4.0.0-pre |
| Object Recognition | Azure Vision (cloud) + Haar cascades (local, disabled) | ⚠️ Cloud-dependent, no local ML |
| Build System | CMake + vcpkg | ✅ Migrated from .vcxproj |
| Platform | Windows only (Service, TCHAR, Sleep, backslash paths) | ⚠️ Sleep/paths fixed, strings partially migrated |
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
| CppREST SDK — archived, limited Linux support | Major | Pending — Phase 2 (Crow) |
| `string_t` / `_T()` / `tcout` / TCHAR everywhere | Major | ⚠️ Partially done — `StringT` typedefs in place |
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

### Known Security Issues to Fix During Migration

| Issue | Severity | Location |
|-------|----------|----------|
| CORS `Access-Control-Allow-Origin: *` on stream endpoints | Critical | `Stream.cpp` lines 67, 84, 109 |
| Session cookie missing `Secure; SameSite=Lax` flags | Critical | `Authenticate.cpp` line 320 |
| `#if !_DEBUG` auth bypass on stream endpoint | High | `Stream.cpp` lines 40-44 |
| No CSP header | Medium | HTTP responses |
| Inline `onclick` handlers in templates | Medium | `preview.html` |

---

## Target Architecture

```
Current:                              Target:
─────────────────────────             ─────────────────────────
WitnessServer.exe (Windows)           witness-server (Linux/Mac/Windows)
├── CppREST SDK (HTTP)                ├── Crow (HTTP + WebSocket)
├── IListenerCommand (routes)         ├── Route handlers
├── string_t / TCHAR                  ├── std::string (UTF-8)
├── Windows Service (SCM)             ├── systemd / launchd / SCM
├── Azure Vision (cloud detection)    ├── ONNX Runtime (local detection)
└── Manual DLLs                       └── vcpkg (all deps)

WitnessCamera.dll                     libwitnesscamera.so / .dylib / .dll
├── Sleep()                           ├── std::this_thread::sleep_for()
├── string_t                          ├── std::string
└── (otherwise already portable)      └── (same pipeline, portable)

Web Frontend:                         Web Frontend:
├── Knockout.js 3.4.2                 ├── Vue 3 (Composition API)
├── Bootstrap 3.3.5                   ├── Bootstrap 5
├── jQuery 2.1.4                      ├── fetch API (no jQuery)
└── No mobile support                 └── PWA + Web Push notifications

Build:                                Build:
├── CMakeLists.txt (root)             ├── CMakeLists.txt (root)  ✅
│   ├── WitnessCamera/CMakeLists.txt  │   ├── WitnessCamera/CMakeLists.txt
│   └── WitnessServer/CMakeLists.txt  │   └── WitnessServer/CMakeLists.txt
├── vcpkg.json (all deps)  ✅        ├── vcpkg.json (all dependencies)
└── (old .vcxproj removed)           └── GitHub Actions CI (Windows + Linux)
```

### Target vcpkg Dependencies

```json
{
  "dependencies": [
    "crow",
    "nlohmann-json",
    "ffmpeg",
    "opencv4",
    "libsodium",
    "sqlite3",
    "onnxruntime"
  ]
}
```

---

## Implementation Phases

### Phase 1: Cross-Platform Foundation

Make the codebase buildable on Linux without changing functionality.

- [x] **Replace `Sleep()` with `std::this_thread::sleep_for()`**
  - 6 call sites across 4 files (`CameraWorker.cpp`, `WatchdogWorker.cpp`, `TimerWorker.cpp`, `Main.cpp`)
  - Removed `#include <windows.h>` from files that only needed it for `Sleep()`
- [x] **Portable path handling**
  - Replaced `CreateDirectory`/`CreateDirectoryW` → `std::filesystem::create_directories()`
  - Replaced backslash path joins → `std::filesystem::path /` operator
  - Replaced `std::tr2::sys::path` (deprecated) → `std::filesystem::path`
- [x] **Introduce `StringT`/`CharT`/`StringStreamT` intermediary types**
  - Defined in `Common.h` as aliases for `utility::string_t` (CppREST)
  - Replaced all direct `string_t` usage across 42 files
  - When Crow replaces CppREST (Phase 2), change the 3 typedefs to `std::string`/`char`/`std::stringstream`
- [x] **CMake migration** — replaced `.vcxproj`/`.sln` with CMake + vcpkg
  - Created `CMakeLists.txt` (root), `WitnessCamera/CMakeLists.txt`, `WitnessServer/CMakeLists.txt`
  - Created `CMakePresets.json` with vcpkg toolchain integration
  - All dependencies via vcpkg: cpprestsdk, ffmpeg, opencv4, libsodium, sqlite3
  - Updated OpenCV from 4.0.0-pre to 4.10+ (fixed deprecated C API usage)
- [ ] **Replace `string_t` / `_T()` / TCHAR with `std::string` (UTF-8)**
  - Incremental approach: first make `string_t` a typedef for `std::string` in `Common.h`, remove `_T()` macros, fix compile errors
  - Then clean up wide-string remnants during Crow migration (Phase 2)
  - This is the biggest mechanical change — touches nearly every file
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

### Phase 2: Replace HTTP Server

CppREST SDK is the primary Linux blocker. Replacing it is required, not optional.

- [ ] **Replace CppREST SDK with Crow**
  - Cross-platform (Linux, Mac, Windows), header-only, vcpkg package
  - Uses `std::string` natively — aligns with Phase 1 string migration
  - Built-in WebSocket support (future: replace long-polling for camera state)
  - JSON via nlohmann/json (cross-platform, modern)
- [ ] Migrate `WitnessListener` → Crow app
- [ ] Migrate `IListenerCommand` subclasses → Crow route handlers
- [ ] Migrate CppREST JSON → nlohmann/json
- [ ] **Fix security issues during migration:**
  - Remove CORS wildcard on stream endpoints → origin whitelist or config
  - Add `Secure; SameSite=Lax` to session cookies
  - Remove `#if !_DEBUG` auth bypass
  - Add CSP header to responses
  - Convert inline `onclick` handlers to framework event bindings

### Phase 3: Local Object Recognition

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
- [ ] **Ship default model** — YOLOv8n (~6MB, ~30ms/frame on CPU)
- [ ] Optionally keep Azure Vision as alternative cloud backend

### Phase 4: Web Frontend Modernization

- [ ] **Replace Knockout.js with Vue 3** (Composition API + single-file components)
  - Vite build toolchain (replaces manual script includes)
  - Vue Router for client-side navigation
  - Pinia for state management (replaces global ViewModel)
- [ ] **Replace Bootstrap 3 with Bootstrap 5**
  - No jQuery dependency
  - Modern responsive grid
  - CSS custom properties for theming
- [ ] **Drop jQuery** — use native `fetch` API for HTTP, `querySelector` for DOM
- [ ] **Add PWA support** — manifest + service worker for installable web app
- [ ] **Port HLS client architecture to Vue**
  - StreamDiagnostics system, poll-based watchdog, exponential backoff
  - Preserve the "never seek from outside HLS.js" rule
  - Fullscreen camera view as dedicated Vue route

### Phase 5: Notifications & Cleanup

- [ ] **Web Push via VAPID** — libsodium for signing (already a dependency, cross-platform)
- [ ] **Notification triggers** from `ONNXDetectionFilter` results via existing `MessageBus`
- [ ] **Evaluate Flutter/Android projects** — PWA may replace them entirely
- [ ] Remove `.vcxproj` files once CMake is proven stable
- [ ] Remove Azure Vision code if ONNX fully replaces it

---

## Key Technical Decisions

### String Migration Strategy

**Recommended: Incremental (Option B)**

1. Phase 1: Make `string_t` typedef to `std::string` in `Common.h`, remove `_T()` macros, fix all compile errors
2. Phase 2: Clean up wide-string remnants during Crow migration

This avoids a massive single PR while keeping the codebase compilable at every step.

### Why Crow Over Alternatives

| | Crow | Drogon | cpp-httplib | Beast |
|---|---|---|---|---|
| Cross-platform | ✅ | ✅ | ✅ | ✅ |
| Header-only | ✅ | ❌ | ✅ | ✅ |
| WebSocket | ✅ | ✅ | ❌ | ✅ |
| Routing DSL | ✅ Simple | ✅ Rich | ❌ Manual | ❌ Manual |
| vcpkg | ✅ | ✅ | ✅ | ✅ (Boost) |
| Learning curve | Low | Medium | Low | High |
| Community | Active | Active | Active | Boost ecosystem |

Crow wins on simplicity — its routing DSL maps cleanly to the existing `IListenerCommand` pattern, and it's header-only so there's no additional build complexity.

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

- **WitnessCamera DLL boundary is a strength** — the video pipeline is already isolated. Cross-platform means building as `.so` (Linux) / `.dylib` (Mac) instead of `.dll`, but the C++ source is almost clean.
- **OpenCV and FFmpeg are natively cross-platform** — switching from pre-built Windows binaries to vcpkg packages makes them work everywhere automatically.
- **Systemd integration on Linux is straightforward** — a `.service` file plus `sigaction()` signal handling replaces the entire Windows SCM layer.
- **The filter architecture is the best part of the codebase for extensibility** — ONNX detection is genuinely a "write one class" task because the pipeline, frame decode, and result structures already exist.
- **Phase ordering is intentional** — each phase is independently useful and shippable. Phase 1+2 unlock Linux. Phase 3 adds local ML. Phase 4+5 modernize the user experience.
