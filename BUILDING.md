# Building Witness

## Deployment identities

Health reports and the console dashboard distinguish the web bundle ID from
the native source/toolchain fingerprint embedded independently in the server
EXE and camera DLL. The native fingerprint includes native sources/headers,
build definitions and dependency manifests, but excludes web assets. It is
generated on each build and its header changes only when those inputs change;
a web-only rebuild therefore leaves native binaries untouched. Native build
configuration is reported separately, and an EXE/camera-DLL fingerprint or
configuration mismatch produces a warning.

These fingerprints are not binary checksums or proof of the installed external
dependency contents. Use SHA-256 file hashes for byte-exact deployment audits.
Deploy the EXE, Witness DLLs and Web folder together; do not identify native
binaries solely by the web bundle ID or filesystem timestamps.

## Stable Windows build

The preferred Windows environment deliberately separates dependency changes
from ordinary configuration and compilation. It uses Visual Studio 2026's
latest installed `v145` compiler while vcpkg stays within the MSVC 14.51 ABI
family, builds third-party
dependencies once in Release mode, and ignores compiler executable churn from
compatible servicing updates.

Because this is a native x64-on-x64 build, vcpkg host tools and target
libraries use the same managed triplet. Otherwise code generators retain the
default compiler-tracked `x64-windows` identity and can still churn after an
IDE update.

Bootstrap dependencies explicitly on a new checkout or after an intentional
manifest, registry, triplet, or vcpkg update:

```powershell
.\scripts\bootstrap-vcpkg.ps1       # first checkout, or when the pin changes
.\scripts\bootstrap-dependencies.ps1
```

This creates `build-vs2026-stable` without touching older `build` or
`build-vs2026` trees. It uses a persistent binary cache at
`%LOCALAPPDATA%\Witness\vcpkg-binary-cache`; set
`WITNESS_VCPKG_BINARY_CACHE` to a shared absolute directory before running the
script to share packages with another machine or CI worker.

After bootstrap, normal work cannot install, remove, or rebuild dependencies:

```powershell
cmake --preset windows-vs2026-stable
.\scripts\build-windows.ps1
```

The scripts prefer Visual Studio 2026's bundled CMake because older CMake
installations do not recognise the VS 2026 generator. Set `WITNESS_CMAKE` to an
explicit executable to override it. Visual Studio itself can configure and
build the same preset normally.

If a pinned input changes, configure stops and names the bootstrap command. It
does not silently accept vcpkg's proposed package churn. Review the diff first,
record why the ABI or package graph changed, and run bootstrap only when that
change is intentional. A compiler servicing update within 14.51 is allowed; a
future compiler ABI family requires changing the triplet and bumping its
documented ABI epoch. Keeping a new triplet/build directory lets the old
environment remain usable until its replacement has successfully built.

The file `build-vs2026-stable/witness-dependency-environment.txt` records the
manifest, registry, triplet, vcpkg, CMake, compiler, generator, and toolset used
for the environment. The first four form the enforced dependency fingerprint;
the others make build reports diagnosable without making harmless IDE/CMake
updates mutate installed packages.

[`build-environment.json`](build-environment.json) is the reviewed source of
truth for the dependency ABI epoch, vcpkg commit, and supported MSVC family.
The dependency bootstrap accepts a matching `VCPKG_ROOT` or integrated vcpkg,
but prefers the ignored repository-local `.build-tools/vcpkg` installed by the
vcpkg bootstrap helper. A machine-wide vcpkg update therefore cannot silently
alter the managed environment.

## Existing and non-Windows builds

Existing build trees and the legacy presets continue to behave as before. The
managed policy is opt-in through `windows-vs2026-stable`, which avoids breaking
the current known-good dependency installation while the new release-only
environment is populated and tested.

Linux remains on the existing CMake/vcpkg path for now. The broader comparison
of CMake/vcpkg, generated native Visual Studio projects, Meson, and Conan is a
separate measured decision; the current build must remain available until an
alternative produces equivalent Windows and Linux artifacts.

## Build-time profiling

Do not restructure headers or add PIMPL solely on intuition. Once the stable
environment is built, capture an incremental profile and then a clean profile:

```powershell
.\scripts\profile-build.ps1
.\scripts\profile-build.ps1 -Clean
```

The script combines an MSBuild binary log, MSVC `/Bt+` and `/d1reportTime`
compiler timings, and a level-3 C++ Build Insights trace collected by
`vcperf /noadmin`. Results are placed under ignored `artifacts/build-profile-*`
directories. Open `build.binlog` in MSBuild Structured Log Viewer and
`build-timetrace.json` in `edge://tracing`. Use the measured translation units,
headers, templates, and build fan-out to prioritize ABI/PIMPL work.

The 2026-09-18 clean RelWithDebInfo baseline contained 69 C++ translation units
and 439 seconds of aggregate compiler front-end work versus 82 seconds in the
back end. The generated projects did not enable MSVC `/MP`, so the trace saw
only two compiler processes even when the build was launched with 18-way
parallelism. Witness now enables `/MP` for C++ compilation; CMake/MSBuild
parallelism continues to control concurrent projects. A subsequent clean
RelWithDebInfo build, including the web bundle and link, completed in 136
seconds on the same 12-logical-CPU development machine.

The next largest measured issue is repeated parsing of the exported camera
header graph. Typical server translation units spent about five seconds in the
front end, repeatedly reaching `OutputStream.h`, `Stream.h`, `RecordFilter.h`,
and `SourceStats.h`. Address that separately with forward declarations, PIMPL,
and then a measured PCH experiment; do not hide the associated C4251 ABI
warnings globally.

On 2026-09-19, isolating the shared segment-buffer type and removing the
`CameraWorker.h` dependency from `GlobalContext.h` let HTTP translation units
include only the camera implementation headers they use. A server-project
RelWithDebInfo rebuild then took 89 seconds (92 seconds after the value-type
export cleanup). A Crow/Asio-only PCH reduced the same project rebuild to 65
seconds on the same machine (about 29% less than the latter baseline); it
does not include camera headers or change the native dependency ABI. The
precompiled-header experiment is enabled for MSVC only.

On a fresh VS2026 RelWithDebInfo project rebuild on 2026-09-19, the Camera
DLL emitted 88 warnings and the server 296. A scoped ABI pass moved STL and
callback state from `InputStream`, `ContinuousOutputStream`, and the existing
`ReolinkUnofficialFilter` PIMPL behind DLL-owned pointers. It also made the
`ImageProcessingJobQueue` forward declaration agree with its `struct`
definition, eliminating C4099. Rebuilding both projects with the same command
and `/t:Rebuild` produced 61 Camera warnings and 193 server warnings (130
fewer in total), with no errors. The remaining compiler warnings are C4251,
concentrated in `LiveOutputStream` and `ReolinkBaichuanClient`. Those classes
own substantially more active streaming/network state and need their own
reviewed PIMPL migration and cross-DLL lifetime tests; do not suppress C4251
globally.

### Profiling safely in an existing dependency environment

Use `scripts/profile-build.ps1 -BuildRoot build-vs2026 -UseExistingConfiguration`
to measure the existing build without switching dependency installations. The
profiler explicitly disables dependency installation/bootstrap, captures verbose
native timing output in `build-output.txt`, and restores the previous
`WITNESS_PROFILE_BUILD` value even when the build fails. `session-output.txt`
records configuration and trace collection separately. Use
`-KeepProfilingConfiguration` only when intentionally retaining timing flags.
`-Clean` is restricted to a build directory beneath this repository; it cleans
project build outputs, not the installed vcpkg package tree.

Summarize a trace without printing captured environment variables or command
lines:

```powershell
python Tools/summarize_build_trace.py artifacts/build-profile-YYYYMMDD-HHMMSS/build-timetrace.json
python -m unittest discover -s Tools/Tests -p test_build_trace_summary.py
```

The 2026-10-04 clean VS2026 RelWithDebInfo profile recorded 79 front-end and
79 back-end passes across 78 distinct source paths, with no dropped trace
events or unmatched begin/end events. Aggregate front-end work was 629.1s and
back-end work 308.7s. These are overlapping compiler durations with heavy
profiling enabled, not build elapsed time or a comparable uninstrumented
baseline. All phase durations were attributed to a source path.

The largest individual phase totals were `CrowRoutes_Debug.cpp` (50.5s),
`CrowListener.cpp` (45.0s), and `CameraWorker.cpp` (36.4s). The Crow PCH took
17.7s. Repeated inclusive project-header timings put `RecordFilter.h` first
(84.2s over 37 visits), followed by `Stream.h` (50.0s over 26 visits) and
`SourceStats.h` (46.3s over 37 visits). Header timings include nested headers;
they cannot be added together or interpreted as the savings from removing
one include. This identifies candidates for a subsequent measured header/PIMPL
pass, not proof that a particular refactor is already faster.

Raw traces and binary logs may contain machine paths, environment variables,
and command lines. Keep those ignored artifacts local and review/redact them
before sharing. The summary omits these fields by design.

### Baichuan DLL ownership pass (2026-10-04)

`ReolinkBaichuanClient` now owns one opaque implementation pointer. Credential,
socket, TLS, thread, atomic, mutex, error and detection state stay inside the
Camera DLL. Exported calls delegate to the existing protocol implementation;
copy/move remain unavailable because an active worker needs a stable address.
This changes the class size: rebuild and deploy the server and Camera DLL
together, never mix a pre-refactor consumer with the new DLL.

The normal RelWithDebInfo before/after build logs contained 264 versus 216
C4251 occurrences, eliminating the 48 occurrences involving this class without
suppressing warnings. The initial normal rebuild took 99.5s, versus 105.0s for
the post-refactor `/t:Rebuild`. Different rebuild mechanisms and single samples
do not establish a compile-time improvement; this pass primarily improves
ownership boundaries and warning health. `LiveOutputStream` remains the next
exported-state migration and needs separate streaming lifetime tests.

```powershell
cmake --build build-vs2026 --config RelWithDebInfo --target BaichuanLifetimeTests --parallel 2
.\build-vs2026\bin\RelWithDebInfo\BaichuanLifetimeTests.exe
```

The test links to the actual Camera DLL. Twenty synthetic loopback TCP cycles
verify initial state, independent returned detection snapshots, idle/repeated
Stop, duplicate Start, and both explicit Stop and destruction during a stalled
encryption-negotiation receive. Each shutdown must finish within three seconds.
Compile-time checks require pointer-sized ownership and prohibit copy/move.
No physical camera, production credentials, PTZ command, or authentication
bypass is used. This does not validate real-camera detections, TLS read/write
concurrency, or Linux runtime behavior. Existing transport/lifecycle behavior,
including TLS cleanup ordering and retry sleep, is unchanged by this pass;
the synthetic TCP result must not be treated as proof of every network path.

### Live-stream DLL ownership pass (2026-10-04)

`LiveOutputStream` owns one immutable pointer to Camera-DLL-private state.
Its segment backlog, FFmpeg resources, capture state, callbacks, cadence guards
and diagnostic rings no longer live in the exported class. Ten formerly inline
accessors/mutators execute in the DLL; `InputStream` and repair machinery no
longer need to be included by the public live-stream header. Delivery evidence
is an independent header-defined value type, preserving the diagnostics schema.
The existing mux/clock method bodies are unchanged apart from accessing the
private state. Copy/move are explicitly unavailable.

The normal RelWithDebInfo DLL/server rebuild completed with **zero C++ compiler
warnings**, versus 216 C4251 occurrences immediately before this pass. Linker
LNK4075 and web Sass warnings remain; they are not hidden. The rebuild took
153.4s, versus the previous 105.0s sample, so this does **not** demonstrate a
compile-time speedup. Timing comparisons need a controlled repeated baseline;
the proven improvement here is warning and DLL ownership health.

```powershell
cmake --build build-vs2026 --config RelWithDebInfo --target LiveStreamOwnershipTests LiveCadencePauseTests LiveTimestampRecoveryTests TimestampRegressionGuardTests --parallel 2
```

Tests use offline fixtures, not real cameras or a browser:

- The exported-DLL ownership test passed eight cycles each with H.264, HEVC,
  and H.264/AAC: callback media retains its original hash after reconnect and
  stream destruction; init snapshots and retained segment copies survive;
  explicit/repeated Shutdown and destructor-only shutdown are exercised. The
  AAC case also requires an audio codec and muxed audio packet evidence.
- Existing cadence tests pass with both H.264 and HEVC across a virtual
  15-second delivery pause, a clean-keyframe generation reset, and a deliberate
  sustained rate mismatch which must still be rejected. The cooldown/recovery
  fixture and timestamp guard regressions also pass.
- All eight emitted control/pause/recovery MP4 generations strictly decode
  with `ffmpeg -err_detect explode -xerror`. Decoded-frame timestamps increase
  monotonically: control/recovery intervals are 50ms, pause outputs stay within
  47.5–52.5ms, matching the production clock's explicit nominal/20 correction
  limit. An initially tighter ±1ms check was inappropriate for that intentional
  correction and was replaced with the actual production bound.

Fixtures and results are ignored under `artifacts/live-pimpl-tests`. These tests
do not establish production/MSE stability or concurrent shutdown safety while
another caller is using the stream; its existing camera-worker ownership
contract is unchanged. Because exported class layout changed, always deploy
the rebuilt server and Camera DLL together.

### Dependency-reuse validation (2026-10-04)

Managed bootstrap permission is now one-shot: its cached flag is consumed
before `project()` can invoke the vcpkg toolchain. Both successful and failed
bootstrap configurations require a fresh explicit request before installation
is allowed again. A stale `VCPKG_MANIFEST_INSTALL=ON` cannot authorize ordinary
managed configuration. The bootstrap wrapper also passes the selected pinned
toolchain explicitly; merely setting `VCPKG_ROOT` does not replace a previously
cached, valid `CMAKE_TOOLCHAIN_FILE`.

The shared production policy is exercised with real CMake and a synthetic
installation marker, not real package installation. Tests cover success/failure
followed by ordinary retries, compiler servicing within exactly `19.51`, rejected
adjacent/malformed families, missing/mismatched/duplicate stamps, target-triplet
identity, and LF/CRLF-equivalent logical fingerprints. Compiler-family dots are
escaped, so `19x51` cannot accidentally match `19.51`.

Complete installed-package content can be checked independently of the source
fingerprint:

```powershell
python Tools/dependency_inventory.py capture build-vs2026/vcpkg_installed artifacts/dependencies-before.json
# Configure/build with dependency installation explicitly disabled.
python Tools/dependency_inventory.py capture build-vs2026/vcpkg_installed artifacts/dependencies-after.json
python Tools/dependency_inventory.py compare artifacts/dependencies-before.json artifacts/dependencies-after.json
python -m unittest discover -s Tools/Tests -p 'test_dependency_*.py'
```

Snapshots hash every regular installed file, including headers, binaries, and
metadata; detect additions/removals/same-size content changes; refuse linked
entries and files changing during hashing; and must be saved outside the
package tree without overwriting an existing snapshot. They prove file-content
reuse, not ACL/permission invariance or binary ABI compatibility by themselves.

On this machine, successful no-install configuration of the current legacy
`build-vs2026` tree retained all 3,078 vcpkg files (1,830,711,343 bytes) and all
26 official ONNX package files (394,357,023 bytes) unchanged. The managed
`build-vs2026-stable` tree rejected its pre-existing stale stamp and retained all
2,673 installed files (759,367,637 bytes) unchanged. Its stamp predates logical
fingerprinting and does not match today's manifest/registry hashes; it was not
silently re-stamped or bootstrapped. A successful real managed migration remains
unverified until its inputs are reviewed and explicitly reconciled. The positive
managed retry behavior is proven by the isolated real-CMake policy tests, not by
claiming that the stale real managed tree configured successfully.

Managed configuration also verifies the **Git checkout**, not just `vcpkg.exe
version`, before loading its toolchain. The checkout root must match the selected
tool root, HEAD must equal the pinned commit, and tracked files must be clean.
Untracked package/download caches are permitted. The executable version is
checked before `project()` too, preventing an explicitly requested bootstrap
from running with mismatched scripts or a stale binary and only noticing later.

The 2026-10-04 global installation illustrates why both checks matter: its
executable reported the expected `98d7cb0...` commit while Git HEAD was
`a1cae005...`. The managed build correctly rejects this installation before
toolchain execution. No Git checkout, fetch, reset, or package bootstrap was
performed as part of validation. The existing legacy build remains available;
it does not gain the full managed policy merely because its packages were
hash-verified.

The dependency wrapper requires a matching clean checkout as well as binary,
and still explicitly selects that toolchain over an old cache entry. The pinned
vcpkg bootstrap helper refuses tracked modifications instead of carrying them
through a checkout, fetches the exact commit from the official repository rather
than trusting an existing `origin`, and validates the commit argument first.
These network/bootstrap paths were not executed during this pass; policy tests
use temporary local Git repositories and a synthetic installation marker.

The MSVC link options now explicitly disable incremental linking, matching the
existing `/OPT:REF` and `/OPT:ICF` behavior rather than requesting incompatible
modes. After CMake regenerates Visual Studio projects, start a fresh MSBuild
invocation: an already-loaded project can retain its old settings for that run.
The regenerated RelWithDebInfo build linked successfully without LNK4075; the
full rebuild had no C++ compiler warnings. Third-party Sass deprecations and the
web bundle size advisory remain separate work, not suppressed warnings.
